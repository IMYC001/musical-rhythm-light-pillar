# Windows 原生版

C++17 + D3D11 + DirectComposition。单文件、无运行时依赖、静态链接。

<div align="center">
<img src="../docs/images/panel_params.png" width="280" alt="参数页">
<img src="../docs/images/panel_sound.png" width="280" alt="声音页">
</div>

## 构建

需要 **llvm-mingw**（自带 clang + mingw-w64 ucrt，不需要 MSVC / Windows SDK）。

```bash
LP_MINGW=/path/to/llvm-mingw/bin ./build.sh        # 主程序 + 全部诊断工具
LP_MINGW=/path/to/llvm-mingw/bin ./build.sh app    # 只编主程序
```

产物在 `build/`：

| 文件 | 说明 |
|---|---|
| `LightPillar.exe` | 主程序，约 580 KB |
| `probe.exe` / `shot.exe` / `cmp.exe` | 诊断工具（见下） |

依赖全是系统自带：`d3d11` `dxgi` `d3dcompiler_47` `ole32` `mmdevapi`
`shell32` `comctl32` `shlwapi` `gdi32` `user32` `gdiplus` `dwmapi` `winmm` `psapi`。

### 关于 `--no-insert-timestamp`

链接参数里有 `-Wl,--no-insert-timestamp`。默认 lld 会把链接时刻写进 PE 头，
同一份源码两次构建 md5 就不同 —— 这会让人怀疑「发布的 exe 和源码对不上」。
关掉之后构建是**字节可复现**的，md5 才是一把真的尺子。

### 主题色表

`src/data_themes.h` 由 `scripts/gen_data.js` 从原作品的 JS bundle 抽取生成。
**这个头文件已经进版本库**，所以没有原作品 bundle 也能编译。

要重新生成（比如原作品更新了主题）：

```bash
LP_BUNDLE=/path/to/index-XXXX.js node scripts/gen_data.js
```

## 运行时文件

程序是绿色的。首次运行在 **exe 同目录**生成：

- `settings.ini` —— 当前参数（27 键 + 机器项）
- `presets.ini` —— 保存的预设

删掉 exe 和这两个文件就彻底干净了，不写注册表、不写 `%APPDATA%`。

## 诊断工具

移植过程中攒了一批「对照台」。它们不是玩具 —— 每一个都对应一次「靠猜猜不出来」
的排查，作用是**把猜测变成可判定的实验**。

| 工具 | 用途 |
|---|---|
| `probe.exe` | 读回覆盖层 framebuffer 的统计量（最亮点 / 轴线采样 / alpha） |
| `shot.exe` | 截屏（含分层窗口） |
| `cmp.exe` | 两张图逐像素比对，输出差异分布 |
| `clickprobe.exe` | 跨进程点击靶：验证「鼠标穿透到底有没有生效」 |
| `hitlab.exe` | 穿透实验台：几种窗口样式组合的命中测试矩阵 |
| `layertest.exe` | 分层渲染验证：确认 DirectComposition 图层真的在合成 |
| `gapwatch.exe` | 纯计时对照探针（不含 D3D），用来分开「我们卡」和「系统卡」 |
| `audiodiag.exe` | 音频链路诊断：设备 / 格式 / 采样率 / 有没有数据 |
| `audiotest.exe` | 分析链对照台：把同一段信号喂进 C++ 版和 JS 参考实现，逐段比 |
| `tonetest.exe` | 生成测试音源（正弦 / 扫频 / 鼓点） |
| `setmute.exe` | 默认设备的静音 / 音量夹具（测「静音时会发生什么」） |
| `rchk.exe` | 环境体检：远程会话 / 分辨率 / 刷新率 |
| `dragtest.exe` | 面板拖动验证（含一个故意写错的对照组） |
| `prompttest.exe` | 单行输入框测试台（重命名预设用） |

用法见 [`tools/README-对照台.md`](tools/README-对照台.md)。

### 两个「故意写错」的对照组

`build.sh` 能编出两个**故意退回旧写法**的版本，用来证明自检查得出对应的 bug：

```bash
./build.sh bugdrag      # 退回「用客户区坐标拖动」→ 面板会抽搐
./build.sh bugpreset    # 退回「用 PresetIo 读预设」→ 设置被默认值冲掉
```

如果自检对这两个版本都报「通过」，说明自检本身是坏的。

## 源码地图

| 文件 | 职责 |
|---|---|
| `src/main.cpp` | 主程序：窗口、托盘、面板装配、预设、参数 IO、自检 |
| `src/d3d.cpp` | D3D11 设备 + DXGI 交换链 + DirectComposition 合成 |
| `src/audio.cpp` | WASAPI loopback 采集 + FFT + 8 段包络 + 自适应归一化 |
| `src/panel.cpp` | 自绘面板（GDI+，两页：声音 / 参数） |
| `src/prompt.cpp` | 单行输入框（重命名预设） |
| `src/shader_src.h` | HLSL 着色器，逐行对应原作品的 GLSL |
| `src/data_themes.h` | 15 套主题色（生成物，不要手改） |

## 已知限制

- 需要 **Windows 10 1803+**（DirectComposition 预乘 alpha 合成）。
- 不接管**独占全屏**的游戏画面 —— 全屏独占时 DWM 不合成桌面层。
  用无边框窗口模式即可。
- 多显示器时按 `displayIndex` 选屏，`-1` = 主屏。
