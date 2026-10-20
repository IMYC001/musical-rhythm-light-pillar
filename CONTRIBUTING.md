# 贡献指南

这个项目的核心约束只有一条：**改动必须可验证**。

它不是「看起来对就行」的视觉效果 —— 每一处几何、每一个参数都有可复现的判据。
提 PR 时请把「你怎么证明它对了」一起写上来。

---

## 提 Issue

请附上：

- 平台（Windows / Android + 版本）
- 屏幕分辨率与缩放
- 相关参数（`settings.ini` 里那几行，或面板截图）
- 如果是「看不见光柱」：先看 [参数说明 · 调坏了怎么救](docs/参数说明.md#七调坏了怎么救)

Android 侧加一条探针日志会非常有用：

```bash
adb logcat -s LightPillar:I
```

---

## 改代码

### 先能编译

```bash
# Windows
cd windows && LP_MINGW=/path/to/llvm-mingw/bin ./build.sh app

# Android
cd android && ./gradlew assembleDebug
```

### 再能验证

Windows 侧有一批「对照台」（见 [`windows/tools/README-对照台.md`](windows/tools/README-对照台.md)），
Android 侧有 `tools/` 下的真机脚本。改动了这些地方请跑对应的验证：

| 改了什么 | 跑什么 |
|---|---|
| 着色器（`shader_src.h` / `Shaders.kt`） | 差分几何度量：左右柱高差 / 射程差应为 0，中线应为空 |
| 音频分析链（`audio.cpp` / `Analyzer.kt`） | `scripts/specdiff.sh` 或 `SelfCheck` |
| 面板 / 参数（`panel.cpp` / `PanelBuilder.kt`） | 逐键对照 `settings.ini`，确认 27 键都能存能读 |
| 窗口 / 合成（`d3d.cpp` / `GlRenderer.kt`） | `probe.exe` 或探针日志：`max=(0,0,0,1)` = 整帧全黑 |

### 两个平台要同步

`windows/src/shader_src.h` 和 `android/app/src/main/java/com/lightpillar/Shaders.kt`
是同一份着色器的两种语言。**改一边必须改另一边**，否则两边的渲染结果会开始分叉，
而分叉之后所有的「逐像素对照」都失去意义。

同理，`Settings.kt` 的 27 键清单和 `main.cpp` 的 `visitAllSettings()` 必须逐字一致 ——
键名、类型、默认值差一个，跨平台对照出来的差异就全是假的。

---

## 提交信息

不用严格遵循 Conventional Commits，但请让第一行说清「改了什么」：

```
修复竖屏下 beamInset 拉到最低时柱子不离场

横向射程 WX 是为 16:9 写死的常数，竖屏下 0.15×0.75=0.1125 < 0.26，
所以永远有光。按纵横比缩放 WX，并让 inset≠0 时进一步收窄。

验证：480x640 上 -0.15 时 probe max=(0,0,0,1)，差分 ≤1。
```

---

## 不要做的事

- **不要「顺手优化」着色器里的写法。** 那些看起来多余的 `smoothstep` / `pow` /
  `+0.66` 基数都是刻意的，理由写在 [移植笔记](docs/移植笔记.md) 里。
- **不要把 `data_themes.h` 手改。** 它由 `scripts/gen_data.js` 从参考实现 bundle 抽取生成。
- **不要在默认值上「凭感觉调」。** 27 个默认值是参考实现的调校结果，
  改一个就会让所有用户的观感变掉。

---

## 授权

提交代码即表示同意以本仓库的 [GPL-3.0 许可](LICENSE) 发布你的贡献。
但请注意：**着色器算法与配色不在本项目的权利范围内**，它们来自参考实现（见 [README · 授权与致谢](README.md#授权与致谢)）。
涉及那部分的改动，请先确认上游的权利状态。
