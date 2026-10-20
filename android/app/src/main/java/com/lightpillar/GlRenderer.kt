package com.lightpillar

import android.graphics.Bitmap
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.util.Log
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * EGL + GLES 2.0 渲染器。只负责「把一根全屏 quad 画出来」这件事。
 *
 * 用 GLES 2.0 而不是 3.0：参考实现的着色器是 GLSL ES 1.00（`varying` +
 * `gl_FragColor`），2.0 直接吃，不需要改写成 `in/out`。
 * 多两个 quad 的负载在 3.0 上也没有任何收益。
 */
class GlRenderer {

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var program = 0
    private var vbo = 0
    private var aPos = -1
    private val uni = HashMap<String, Int>()

    private var vw = 0
    private var vh = 0

    val isReady: Boolean get() = program != 0

    /** 建 EGL 环境并编译着色器。失败返回 false，[lastError] 里有原因。 */
    fun init(surface: Surface, width: Int, height: Int): Boolean {
        vw = width
        vh = height

        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            lastError = "eglGetDisplay 失败"
            return false
        }
        val ver = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)) {
            lastError = "eglInitialize 失败"
            return false
        }

        val cfgAttribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val numCfg = IntArray(1)
        if (!EGL14.eglChooseConfig(eglDisplay, cfgAttribs, 0, configs, 0, 1, numCfg, 0) ||
            numCfg[0] < 1 || configs[0] == null
        ) {
            lastError = "eglChooseConfig 没找到合适的配置"
            return false
        }

        val ctxAttribs = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
        eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, ctxAttribs, 0)
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            lastError = "eglCreateContext 失败"
            return false
        }

        eglSurface = EGL14.eglCreateWindowSurface(
            eglDisplay, configs[0], surface, intArrayOf(EGL14.EGL_NONE), 0
        )
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            lastError = "eglCreateWindowSurface 失败（Surface 可能已失效）"
            return false
        }

        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            lastError = "eglMakeCurrent 失败"
            return false
        }

        return buildProgram()
    }

    var lastError: String = ""
        private set

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(s)
            Log.e("LightPillar", "着色器编译失败: $log")
            lastError = "着色器编译失败: $log"
            GLES20.glDeleteShader(s)
            return 0
        }
        return s
    }

    private fun buildProgram(): Boolean {
        val vs = compile(GLES20.GL_VERTEX_SHADER, Shaders.VERT)
        if (vs == 0) return false
        val fs = compile(GLES20.GL_FRAGMENT_SHADER, Shaders.FRAG)
        if (fs == 0) {
            GLES20.glDeleteShader(vs)
            return false
        }

        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) {
            lastError = "program 链接失败: ${GLES20.glGetProgramInfoLog(p)}"
            GLES20.glDeleteProgram(p)
            GLES20.glDeleteShader(vs)
            GLES20.glDeleteShader(fs)
            return false
        }
        GLES20.glDeleteShader(vs)
        GLES20.glDeleteShader(fs)
        program = p

        /* 全屏两个三角形（NDC）。顺时针/逆时针都无所谓 ——
           这里不开背面剔除，两种绕序都能过。 */
        val quad = floatArrayOf(
            -1f, -1f, 1f, -1f, -1f, 1f,
            -1f, 1f, 1f, -1f, 1f, 1f,
        )
        val buf: FloatBuffer = ByteBuffer.allocateDirect(quad.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
        buf.put(quad).position(0)

        val ids = IntArray(1)
        GLES20.glGenBuffers(1, ids, 0)
        vbo = ids[0]
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
        GLES20.glBufferData(
            GLES20.GL_ARRAY_BUFFER, quad.size * 4, buf, GLES20.GL_STATIC_DRAW
        )
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)

        aPos = GLES20.glGetAttribLocation(program, "aPos")
        for (n in UNIFORM_NAMES) uni[n] = GLES20.glGetUniformLocation(program, n)

        /* ★★★ 混合方程必须是 GL_ONE / GL_ONE_MINUS_SRC_ALPHA（预乘色的 src-over），
           **不能**用 GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA。
           踩过的坑（真机 480×640 上「光柱完全不显示」的根因）：
             着色器输出的是**预乘色** —— RGB = col（满亮）、A = col * uAlphaK。
             渲染目标先被清成 (0,0,0,0)，于是
               错误写法(SRC_ALPHA)  ： fb.rgb = col*A + 0*(1-A) = 0.1*col²
                  → col=0.5 时只剩 6/255，肉眼完全看不见（桌面差分图上一片纯黑）
               正确写法(ONE)       ： fb.rgb = col   ← RGB 原样落进 framebuffer
             「预乘色要用 GL_ONE 当源因子」是这条管线唯一的正确解 —— 预乘的意思
             就是「RGB 里已经乘过 A 了」，再乘一次 A 就是二次衰减。
           ⚠ 另一条容易被误信的直觉：「不开混合会把光柱外面写成黑色不透明」。
             不成立 —— 光柱外面 col=0 → A 只有 0.004（保险值），本来就是近乎全透明。
             真正决定「外面黑不黑」的是着色器输出的 A，不是开不开混合。 */
        GLES20.glEnable(GLES20.GL_BLEND)
        GLES20.glBlendFunc(GLES20.GL_ONE, GLES20.GL_ONE_MINUS_SRC_ALPHA)
        GLES20.glDisable(GLES20.GL_DEPTH_TEST)
        return true
    }

    fun setViewport(w: Int, h: Int) {
        vw = w
        vh = h
    }

    /**
     * ★ 诊断探针：把当前 framebuffer 读回来。
     *
     * 为什么必须有它：真机上「光柱不显示」有两个完全不同的成因 ——
     *   ① 着色器算出来就是黑的（参数/音频问题）
     *   ② 着色器算对了，但被合成器丢了（预乘 alpha / 硬件合成器问题）
     * 光看屏幕截图**无法区分这两者**，因为两者都表现为「什么都看不见」。
     * glReadPixels 读的是渲染线程自己刚画完的那块 buffer，绕开合成器，
     * 于是能把两半切开：读数非零 + 屏幕全黑 ⇒ 一定是 ②。
     *
     * 返回 [maxR,maxG,maxB,maxA, 左轴心R, 左轴心G, 左轴心B, 左轴心A]。
     * 注意 GL 的原点在**左下角**，所以轴心取样取 y = h/2（竖直中点，两种原点下都一样）。
     */
    fun probe(): IntArray {
        val w = vw; val h = vh
        if (w <= 0 || h <= 0 || eglSurface == EGL14.EGL_NO_SURFACE) return IntArray(10)
        val buf = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.nativeOrder())
        buf.rewind()
        GLES20.glReadPixels(0, 0, w, h, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, buf)
        val out = IntArray(10)
        var mx = -1
        var y = 0
        while (y < h) {
            var x = 0
            while (x < w) {
                val i = (y * w + x) * 4
                val r = buf.get(i).toInt() and 0xff
                val g = buf.get(i + 1).toInt() and 0xff
                val b = buf.get(i + 2).toInt() and 0xff
                val a = buf.get(i + 3).toInt() and 0xff
                if (r > out[0]) out[0] = r
                if (g > out[1]) out[1] = g
                if (b > out[2]) out[2] = b
                if (a > out[3]) out[3] = a
                val lum = r + g + b
                if (lum > mx) { mx = lum; out[8] = x; out[9] = y }   // GL 原点在左下
                x += 4
            }
            y += 4
        }
        /* 左轴心：x=2（最左边两列），竖直中点。右轴心：x=w-3。 */
        val yMid = h / 2
        val li = (yMid * w + 2) * 4
        val ri = (yMid * w + (w - 3)) * 4
        for (k in 0 until 3) {
            out[4 + k] = buf.get(li + k).toInt() and 0xff
        }
        out[7] = buf.get(ri + 3).toInt() and 0xff
        return out
    }

    /** 画一帧。所有参数由 [p] 打包传入。 */
    fun draw(p: FrameParams) {
        if (program == 0) return
        GLES20.glViewport(0, 0, vw, vh)
        /* 清成**全透明**，不是黑色。悬浮窗外面那部分必须让桌面透出来。 */
        GLES20.glClearColor(0f, 0f, 0f, 0f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

        GLES20.glUseProgram(program)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, 0)

        u2("uRes", vw.toFloat(), vh.toFloat())
        u1("uTime", p.time)
        u1("uStyleMode", p.styleMode)
        u1("uBeamInset", p.beamInset)
        u1("uAudioIntensity", p.audioIntensity)
        u1("uResponseRange", p.responseRange)
        u1("uIdleWave", p.idleWave)
        u1("uSubBass", p.subBass)
        u1("uBass", p.bass)
        u1("uLowMid", p.lowMid)
        u1("uMid", p.mid)
        u1("uHighMid", p.highMid)
        u1("uEnergy", p.energy)
        u1("uPresence", p.presence)
        u1("uBrilliance", p.brilliance)
        u1("uAir", p.air)
        u1("uBrightness", p.brightness)
        u3("uCoolCore", p.cool)
        u3("uWarmCore", p.warm)
        u3("uPeakColor", p.peak)
        u1("uGlowIntensity", p.glow)
        u1("uPeakEnabled", p.peakEnabled)
        u1("uPeakIntensity", p.peakIntensity)
        u1("uAlphaK", p.alphaK)
        u1("uDebugMode", p.debugMode)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 6)

        GLES20.glDisableVertexAttribArray(aPos)
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0)
        GLES20.glUseProgram(0)
    }

    /** 交换缓冲。返回 false 表示 EGL surface 已失效（要重建）。 */
    fun swap(): Boolean {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY || eglSurface == EGL14.EGL_NO_SURFACE) return false
        return EGL14.eglSwapBuffers(eglDisplay, eglSurface)
    }

    private fun u1(n: String, v: Float) {
        val l = uni[n] ?: -1
        if (l >= 0) GLES20.glUniform1f(l, v)
    }

    private fun u2(n: String, a: Float, b: Float) {
        val l = uni[n] ?: -1
        if (l >= 0) GLES20.glUniform2f(l, a, b)
    }

    private fun u3(n: String, v: FloatArray) {
        val l = uni[n] ?: -1
        if (l >= 0) GLES20.glUniform3f(l, v[0], v[1], v[2])
    }

    /**
     * ★ 把当前 framebuffer 导出成 PNG 字节（诊断用）。
     *
     * 为什么需要它：[probe] 只给出「最大值」一个数字，定位不了光柱画在哪 ——
     * 真机上遇到的正是这种局面：probe 说 buffer 里有 (98,61,129) 的像素，
     * 屏幕上却一处都没有。只靠数字无法判断是「画错位置」还是「没上屏」。
     * 把 buffer 原样导成图片，一眼就能看出光柱的位置和形状。
     *
     * ⚠ glReadPixels 的原点在**左下角**，Bitmap 在左上角 → 必须上下翻转，
     *   否则导出的图是倒的，看位置会看反。
     */
    fun dumpFramebuffer(): ByteArray? {
        val w = vw; val h = vh
        if (w <= 0 || h <= 0 || eglSurface == EGL14.EGL_NO_SURFACE) return null
        val buf = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.nativeOrder())
        buf.rewind()
        GLES20.glReadPixels(0, 0, w, h, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, buf)
        val px = IntArray(w * h)
        for (y in 0 until h) {
            val sy = h - 1 - y
            val rowBase = sy * w * 4
            for (x in 0 until w) {
                val i = rowBase + x * 4
                val r = buf.get(i).toInt() and 0xff
                val g = buf.get(i + 1).toInt() and 0xff
                val b = buf.get(i + 2).toInt() and 0xff
                val a = buf.get(i + 3).toInt() and 0xff
                px[y * w + x] = (a shl 24) or (r shl 16) or (g shl 8) or b
            }
        }
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        bmp.setPixels(px, 0, w, 0, 0, w, h)
        val bos = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.PNG, 100, bos)
        bmp.recycle()
        return bos.toByteArray()
    }

    fun release() {
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(
                eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT
            )
            if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
            if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
            EGL14.eglTerminate(eglDisplay)
        }
        eglSurface = EGL14.EGL_NO_SURFACE
        eglContext = EGL14.EGL_NO_CONTEXT
        eglDisplay = EGL14.EGL_NO_DISPLAY
        program = 0
    }

    private companion object {
        val UNIFORM_NAMES = arrayOf(
            "uRes", "uTime", "uStyleMode", "uBeamInset",
            "uAudioIntensity", "uResponseRange", "uIdleWave",
            "uSubBass", "uBass", "uLowMid", "uMid", "uHighMid",
            "uEnergy", "uPresence", "uBrilliance", "uAir", "uBrightness",
            "uCoolCore", "uWarmCore", "uPeakColor",
            "uGlowIntensity", "uPeakEnabled", "uPeakIntensity", "uAlphaK", "uDebugMode",
        )
    }
}

/** 一帧要传给着色器的全部量。 */
class FrameParams {
    var time = 0f
    var styleMode = 0f
    var beamInset = 0f
    var audioIntensity = 1f
    var responseRange = 1f
    var idleWave = 0f
    var subBass = 0f
    var bass = 0f
    var lowMid = 0f
    var mid = 0f
    var highMid = 0f
    var energy = 0f
    var presence = 0f
    var brilliance = 0f
    var air = 0f
    var brightness = 0f
    var cool = floatArrayOf(0.35f, 0.10f, 0.90f)
    var warm = floatArrayOf(0.65f, 0.25f, 1.00f)
    var peak = floatArrayOf(1.00f, 0.55f, 0.05f)
    var glow = 1f
    var peakEnabled = 1f
    var peakIntensity = 1f
    var alphaK = 0.10f
    /** ★ 诊断模式：0 = 正常；1/2/3 = 纯红/绿/白不透明。见 Shaders.FRAG 头部注释。 */
    var debugMode = 0f
}
