package com.lightpillar

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.projection.MediaProjection
import android.util.Log

/**
 * 采集到的交错 PCM 的滑动窗口。
 *
 * 语义和 Windows 版 audio.cpp 里的 `pending_` 一致：**永远保留最近一段**，
 * 分析层每次读最后 2048 帧。用 arraycopy 做滑动，不是逐元素取模。
 */
class PcmWindow(private val capacity: Int) {
    private val buf = FloatArray(capacity)
    private var filled = 0
    private val lock = Any()

    fun append(src: FloatArray, off: Int, len: Int) {
        if (len <= 0) return
        synchronized(lock) {
            if (len >= capacity) {
                System.arraycopy(src, off + len - capacity, buf, 0, capacity)
                filled = capacity
                return
            }
            if (filled + len > capacity) {
                val drop = filled + len - capacity
                System.arraycopy(buf, drop, buf, 0, filled - drop)
                filled -= drop
            }
            System.arraycopy(src, off, buf, filled, len)
            filled += len
        }
    }

    /** 取最近 n 个采样写到 out 开头，返回实际取到的数量。 */
    fun snapshot(out: FloatArray, n: Int): Int {
        synchronized(lock) {
            val take = if (n < filled) n else filled
            if (take <= 0) return 0
            System.arraycopy(buf, filled - take, out, 0, take)
            return take
        }
    }

    fun clear() {
        synchronized(lock) {
            filled = 0
        }
    }
}

/**
 * 音频采集。两种来源，产出的都是「交错 float PCM」交给 [Analyzer]。
 *
 * ┌─────────────┬──────────────────────────────────────────────────────┐
 * │ MIC         │ 麦克风。RECORD_AUDIO 运行时授权，零摩擦，一定有反应。  │
 * │             │ 手机外放时「放音乐 → 麦克风听到 → 光柱动」链路是通的。 │
 * ├─────────────┼──────────────────────────────────────────────────────┤
 * │ INTERNAL    │ 系统内媒体音频。Android 10+，走 MediaProjection。      │
 * │             │ ⚠ 每次启动要弹「开始录制或投放」系统对话框；          │
 * │             │   且**播放方可以拒绝被采集**（视频 App / 通话 / 系统音 │
 * │             │   基本采不到，主流音乐 App 可以）。                   │
 * └─────────────┴──────────────────────────────────────────────────────┘
 *
 * ⚠ Windows 版抓的是 WASAPI loopback（整个系统输出）。安卓上**没有等价物**：
 *   REMOTE_SUBMIX 需要系统签名权限 CAPTURE_AUDIO_OUTPUT，普通应用直接
 *   SecurityException。所以「内部音频」这一档天生比 Windows 版弱一头，
 *   这是平台能力的差别，不是实现问题。
 *
 * ⚠ 曾经有过第三档「合成测试音」（逐字移植原生版 tonetest.cpp），
 *   用途是在真机上不看音乐就确认整条链路通。**已按要求删除** ——
 *   需要让画面动起来做诊断时，用 `[android] debugMode = 12/13/14`
 *   （固定 drive = 0.90 / 0.20 / 0.10），那个不依赖音频、更确定。
 */
class AudioEngine {

    enum class Source { MIC, INTERNAL }

    companion object {
        private const val TAG = "LightPillar"
        /** 窗口给足 4 个 FFT 长度（和 Windows 版 pending_ 的上限同量级） */
        const val WINDOW_SAMPLES = LP_FFT_SIZE * 8
    }

    val window = PcmWindow(WINDOW_SAMPLES)

    @Volatile var source: Source = Source.MIC
        private set
    @Volatile var running = false
        private set
    @Volatile var lastError: String? = null
        private set

    /** 实际生效的采样率/声道数（由 AudioRecord 决定，喂给 Analyzer） */
    @Volatile var sampleRate = 48000
        private set
    @Volatile var channels = 1
        private set

    private var thread: Thread? = null

    @Volatile private var quit = false

    private var mediaProjection: MediaProjection? = null

    /**
     * 起采集。
     *
     * @param src       来源
     * @param projection INTERNAL 时必须提供
     * @return 成功返回 true；失败时 [lastError] 有原因
     */
    fun start(src: Source, projection: MediaProjection? = null): Boolean {
        stop()
        source = src
        mediaProjection = projection
        lastError = null
        quit = false
        window.clear()

        if (src == Source.INTERNAL) {
            val rec = openInternal(projection) ?: return false
            return runRecord(rec)
        }

        // MIC：逐档回退。不同设备支持的采样率/编码差别很大。
        val rec = openMic() ?: return false
        return runRecord(rec)
    }

    fun stop() {
        quit = true
        thread?.let {
            it.interrupt()
            try {
                it.join(1200)
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        thread = null
        running = false
    }

    /* ==================== 麦克风 ==================== */

    private fun openMic(): AudioRecord? {
        /* ⚠ 用 CHANNEL_IN_MONO：很多设备不支持麦克风立体声采集，
           请求 STEREO 会直接构造失败。反正分析链对左右声道取的是同一路信号
           （见 Analyzer.computeSpectrum128：channels==1 时 right 复制 left）。 */
        val candidates = listOf(
            Triple(48000, AudioFormat.ENCODING_PCM_FLOAT, AudioFormat.CHANNEL_IN_MONO),
            Triple(48000, AudioFormat.ENCODING_PCM_16BIT, AudioFormat.CHANNEL_IN_MONO),
            Triple(44100, AudioFormat.ENCODING_PCM_16BIT, AudioFormat.CHANNEL_IN_MONO),
        )
        for ((sr, enc, chMask) in candidates) {
            try {
                val minBuf = AudioRecord.getMinBufferSize(sr, chMask, enc)
                if (minBuf <= 0) continue
                val bufSize = minBuf * 4
                @Suppress("MissingPermission")
                val rec = AudioRecord(
                    MediaRecorder.AudioSource.MIC, sr, chMask, enc, bufSize
                )
                if (rec.state != AudioRecord.STATE_INITIALIZED) {
                    rec.release()
                    continue
                }
                sampleRate = sr
                channels = 1
                Log.i(TAG, "麦克风已打开: ${sr}Hz enc=$enc")
                return rec
            } catch (e: Exception) {
                Log.w(TAG, "麦克风打开失败 sr=$sr enc=$enc", e)
            }
        }
        lastError = "没有可用的麦克风配置（可能未授权 RECORD_AUDIO）"
        return null
    }

    /* ==================== 内部音频 ==================== */

    private fun openInternal(projection: MediaProjection?): AudioRecord? {
        if (projection == null) {
            lastError = "内部音频需要先授权「录制或投放」"
            return null
        }
        try {
            val cfg = AudioPlaybackCaptureConfiguration.Builder(projection)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build()

            val sr = 48000
            val chMask = AudioFormat.CHANNEL_IN_STEREO
            val enc = AudioFormat.ENCODING_PCM_16BIT
            val minBuf = AudioRecord.getMinBufferSize(sr, chMask, enc)
            val bufSize = if (minBuf > 0) minBuf * 4 else sr * 2

            val rec = AudioRecord.Builder()
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(enc)
                        .setSampleRate(sr)
                        .setChannelMask(chMask)
                        .build()
                )
                .setBufferSizeInBytes(bufSize)
                .setAudioPlaybackCaptureConfig(cfg)
                .build()

            if (rec.state != AudioRecord.STATE_INITIALIZED) {
                rec.release()
                lastError = "内部音频 AudioRecord 初始化失败"
                return null
            }
            sampleRate = sr
            channels = 2
            Log.i(TAG, "内部音频已打开: ${sr}Hz")
            return rec
        } catch (e: Exception) {
            Log.e(TAG, "内部音频打开失败", e)
            lastError = "内部音频打开失败：${e.message}"
            return null
        }
    }

    /* ==================== 采集循环 ==================== */

    private fun runRecord(rec: AudioRecord): Boolean {
        return try {
            rec.startRecording()
            if (rec.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                rec.release()
                lastError = "AudioRecord 没能进入录制状态"
                return false
            }
            running = true
            thread = Thread { recordLoop(rec) }.also { it.isDaemon = true; it.start() }
            true
        } catch (e: Exception) {
            Log.e(TAG, "启动采集失败", e)
            try {
                rec.release()
            } catch (_: Exception) {
            }
            lastError = "启动采集失败：${e.message}"
            false
        }
    }

    private fun recordLoop(rec: AudioRecord) {
        /* ⚠ AudioRecord.format 返回的是 AudioFormat **对象**（API 23+），
           不是老版本那个 Int 编码常量。写成 `rec.format == ENCODING_PCM_FLOAT`
           编译器会报 Operator '==' cannot be applied to 'AudioFormat' and 'Int'。 */
        val isFloat = rec.format.encoding == AudioFormat.ENCODING_PCM_FLOAT
        /* 每次读 ~20ms。太小会忙等、太大会让包络变钝。 */
        val chunkFrames = (sampleRate / 50).coerceAtLeast(256)
        val chunk = chunkFrames * channels

        val fbuf = FloatArray(chunk)
        val sbuf = ShortArray(chunk)

        try {
            while (!quit) {
                val n: Int
                if (isFloat) {
                    n = rec.read(fbuf, 0, chunk, AudioRecord.READ_BLOCKING)
                    if (n > 0) window.append(fbuf, 0, n)
                } else {
                    n = rec.read(sbuf, 0, chunk, AudioRecord.READ_BLOCKING)
                    if (n > 0) {
                        for (i in 0 until n) fbuf[i] = sbuf[i] / 32768f
                        window.append(fbuf, 0, n)
                    }
                }
                if (n < 0) {
                    Log.w(TAG, "AudioRecord.read 返回 $n")
                    if (n == AudioRecord.ERROR_INVALID_OPERATION || n == AudioRecord.ERROR_DEAD_OBJECT) break
                }
            }
        } catch (e: Exception) {
            if (!quit) Log.e(TAG, "采集循环异常", e)
        } finally {
            try {
                rec.stop()
            } catch (_: Exception) {
            }
            try {
                rec.release()
            } catch (_: Exception) {
            }
            running = false
        }
    }
}