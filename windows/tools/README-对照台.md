# 音频分析链对照台（怎么证明「原生 = 原作品」）

这个目录里的三件东西，是为了回答一个**只能靠数字回答**的问题：

> 用户说：「和 electron 版本的观感相比差了不少，原生的看起来怪怪的，
> 在全部参数相同的情况下，原生的响应幅度没有 electron 大，
> 而且有部分节拍也不会响应。」

「看着不太跳」「有些鼓点没反应」这类描述没法直接定位 bug ——
它可能出在平滑系数、调用频率、频段划分、上采样的任何一环。
所以这里搭了一个**确定性对照台**：把同一串频谱输入喂给两条分析链，逐帧比数字。

---

## 一、结论先说

| 项 | 结果 |
|---|---|
| 原生分析链 vs 原作品 `getAudioData`（60 Hz） | **逐帧一致**，最大绝对差 `0.000e+00` |
| 原生分析链 vs 原作品 `getAudioData`（250 Hz） | **逐帧一致**，最大绝对差 `0.000e+00` |
| 桌面版（Electron）里分析链的**实际频率** | **249.6 次/秒**（实测，不是推断） |
| 修复前（分析跑 60 Hz）的包络幅度 | 只有桌面版的 **70%**（bass）/ 69%（subBass） |
| 修复后（分析跑 250 Hz） | **100%** |

---

## 二、三条频率，别再搞混

原作品的音频链有**三层独立的时钟**，这是整件事的关键：

| 层 | 频率 | 出处 | 干什么 |
|---|---|---|---|
| ① 频谱（数据） | **30 Hz** | `browser-bridge.js`: `var PUSH_HZ = 30;` | FFT → 每声道 64 段 → 128 个频谱值；滚动直方图挂在它上面 |
| ② 分析（包络） | **250 Hz** | bundle: `ke = setInterval(ht, 1e3/1e3)` → 请求 1ms，Chromium 钳到 4ms | 128→512 上采样 → 8 频段和 → 16 个量的一阶平滑 |
| ③ 渲染 | `general.fps`（默认 60） | bundle: `<Canvas frameloop="demand">` + `iG fpsLimit={e}`，`e` 来自 `applyGeneralProperties({fps})` | 把 uniform 喂给着色器 |

**②为什么必须是 250 Hz**：平滑系数是

```js
const ee = r > 0 ? .15 : .08;
```

它是「**每次调用作用一次**」的固定系数，**没有按 dt 缩放**
（同一个函数里的节拍检测器倒是按 dt 缩放了：`1 - Math.pow(.6, r*60)`，
说明作者是有意区分这两者的）。所以调用频率直接决定包络的时间常数：

```
τ = -1 / (f · ln(1 - ee)) = -1 / (f · ln 0.85)

250 Hz → 24.6 ms     ← 桌面版
 60 Hz → 111  ms     ← 慢 4.5 倍
 30 Hz → 223  ms     ← 慢 9 倍
```

低通变慢 = **幅度被压小 + 瞬态被抹平** —— 这就是用户看到的两件事。

---

## 三、桌面版到底跑多快？——实测，不猜

`setInterval(ht, 1e3/1e3)` 请求的是 1ms，但 Chromium 对嵌套定时器有下限钳制。
到底是 60 还是 250，差 4 倍，不能靠读代码推断。

量法：`lp-shell.js` 里包一层 `setInterval`/`setTimeout`，
对 `delay ≤ 10ms` 的回调计数（探针在 bundle 之前执行，所以拦得住）。

```bash
cd lightpillar
node scripts/build-web.js
./node_modules/.bin/electron . --lp-rateprobe 6
```

实测输出：

```
[rateprobe] 统计窗口 6.0017 秒　rAF ≈ 88.6 fps
[rateprobe] 短周期定时器（delay ≤ 10ms）实际频率，按频率降序：
[rateprobe]   delay=1ms  调用 1498 次  →  249.6 次/秒
```

→ **249.6 次/秒**，即 Chromium 的 4ms 钳制。

---

## 四、对照台怎么用

### 1. 取样：录一段真实的 128 个频谱值

```bash
# 一边放测试音、一边录
./build/tonetest.exe 26 &
sleep 2
./build/光柱.exe --dumpspec <绝对路径>/drum.csv drum --hold 20
```

> ⚠ 不用管默认播放设备是不是静音。实测过：静音 + 音量 0 时 WASAPI 环回
> **照样**拿到完整信号（峰值 0.671，与解除静音时一模一样）——
> 环回取的是混音器的输出，不受音量控制影响。
> 真正会让环回「什么都没收到」的是**根本没有声音在播**：
> 这时环回端点不产包，`audiodiag.exe` 会报「包数=0」。
> 需要读/改设备状态时用 `setmute.exe`。

产物格式（`--dumpspec` 直接挂在 `computeSpectrum128()` 末尾，
所以**一帧都不会漏**；在渲染循环里轮询是会漏帧的）：

```
#spec128 label=drum hz=30 bins=128
drum,0.000123456,0.000234567,...,0.000012345      ← 每行 = 一帧频谱（128 值）
```

不接音频也能跑：`python tools/make_synth_dump.py out.csv 12`
会造一份「低频鼓 + 中频持续音 + 高频镲」的合成频谱 ——
合成信号有已知答案，用来先校准对照台本身。

### 2. 两条链各跑一遍

```bash
# 原生侧（C++，src/audiotest.cpp）
./build/audiotest.exe drum.csv native250.csv 250

# 原作品侧（Node，tools/jsref.js + tools/orig-analyzer.js）
node tools/jsref.js drum.csv orig250.csv 250
```

`tools/orig-analyzer.js` **不是手抄的** ——
它由 `tools/extract_analyzer.py` 从原作品 bundle 里按大括号配对
**逐字**切出 `class uC`（节拍触发器）和 `class eG`（音频分析器），
一个字都没改。所以比对结果里不会有「我抄错了」这种噪声。

### 3. 比

```bash
python tools/specdiff.py native250.csv orig250.csv          # 逐帧差
python tools/specdiff.py --peaks n30.csv n60.csv n250.csv   # 各频率下的峰值
```

两边用的是**同一个循环语义**（`audiotest.cpp` 与 `jsref.js` 的注释里写死了）：

```
dt = 1/分析频率,  specDt = 1/30
specAccum = 0,  row = 0,  cur = 全 0     ← 两边的初始状态都是 0
while (row < 行数):
    specAccum += dt
    while (specAccum >= specDt && row < 行数):  cur = rows[row++]
    analyze(cur)
    导出 16 个量
```

---

## 五、实测数字（证据）

### 逐帧比对

```
$ python tools/specdiff.py cmp1/drum-n250.csv cmp1/drum-o250.csv --quiet
帧数 4717　最大绝对差 0.000e+00　容差 2e-04
  bass       峰值比 原生/原作品 = 1.0000
  subBass    峰值比 原生/原作品 = 1.0000
  lowMid     峰值比 原生/原作品 = 1.0000
  energy     峰值比 原生/原作品 = 1.0000
判定: 逐帧一致 ✓
```

（合成 dump 12 秒 / 3000 帧、真实 dump 19 秒 / 4717 帧，两个频率下都是 0。）

### 分析频率 → 包络峰值

合成 dump（`cmp1/n*.csv`）：

| 分析频率 | bass 峰值 | subBass 峰值 | 相对桌面版 |
|---|---|---|---|
| 30 Hz | 0.2706 | 0.4202 | 60% |
| 60 Hz | 0.3139 | 0.4883 | **70%** |
| 120 Hz | 0.3843 | 0.5991 | 85% |
| **250 Hz** | **0.4508** | **0.7037** | 100% |

真实 dump（`cmp1/drum-n*.csv`）：

| 分析频率 | bass 峰值 | subBass 峰值 |
|---|---|---|
| 30 Hz | 0.0769 | 0.1068 |
| 60 Hz | 0.0919 | 0.1244 |
| 120 Hz | 0.1026 | 0.1371 |
| **250 Hz** | **0.1057** | **0.1404** |

### 端到端交叉验证

离线对照台在 250 Hz 下预测 `bass = 0.1057 / subBass = 0.1404`；
真实运行的 `光柱.exe --selftest` 报告 `bass = 0.105 / subBass = 0.138`。
**吻合** —— 说明运行时的分析频率确实已经是 250 Hz，
而且离线对照台忠实复现了运行时行为。
（若按 60 Hz，预测是 0.0919 / 0.1244，和实测明显不符。）

---

## 六、文件清单

| 文件 | 作用 |
|---|---|
| `extract_analyzer.py` | 从原作品 bundle **逐字**抠出 `uC` / `eG` → `orig-analyzer.js` |
| `orig-analyzer.js` | 自动生成，**不要手改**。原作品分析链的参考实现 |
| `jsref.js` | 用 `orig-analyzer.js` 跑 dump，导出 16 个量 |
| `make_synth_dump.py` | 造合成频谱 dump（校准对照台用） |
| `specdiff.py` | 逐帧 diff / 各频率峰值表 |
| `../src/audiotest.cpp` | 原生侧：跑 dump，导出 16 个量 |
