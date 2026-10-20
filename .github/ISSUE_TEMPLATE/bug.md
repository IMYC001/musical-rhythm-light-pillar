name: Bug 报告
description: 光柱不显示、参数没反应、闪退等
labels: ["bug"]
body:
  - type: markdown
    attributes:
      value: |
        先看一眼 [参数说明 · 调坏了怎么救](https://github.com/IMYC001/musical-rhythm-light-pillar/blob/main/docs/参数说明.md#七调坏了怎么救) ——
        一半的「看不见光柱」只是驱动量太低。

  - type: dropdown
    id: platform
    attributes:
      label: 平台
      options:
        - Windows
        - Android
        - 两个都有
    validations:
      required: true

  - type: input
    id: version
    attributes:
      label: 版本
      description: Release 页上的 tag，例如 v1.0.0
      placeholder: v1.0.0
    validations:
      required: true

  - type: input
    id: display
    attributes:
      label: 屏幕分辨率与缩放
      description: Windows 写「2560x1440 @150%」；Android 写机型 + 分辨率
      placeholder: 2560x1440 @150%
    validations:
      required: true

  - type: textarea
    id: what
    attributes:
      label: 现象
      description: 发生了什么，期望是什么
    validations:
      required: true

  - type: textarea
    id: params
    attributes:
      label: 相关参数
      description: |
        Windows：exe 同目录的 `settings.ini` 内容（可以只贴改动过的那几行）
        Android：面板截图，或 `adb shell run-as com.lightpillar cat files/settings.ini`
      render: ini

  - type: textarea
    id: probe
    attributes:
      label: 探针日志（可选，但很有效）
      description: |
        Android：`adb logcat -s LightPillar:I` 的最后几行
        Windows：`probe.exe` 的输出
      render: text

  - type: checkboxes
    id: checks
    attributes:
      label: 确认
      options:
        - label: 已经试过「恢复默认设置」
        - label: 已经确认音频在播放（不是静音 / 不是被独占）
