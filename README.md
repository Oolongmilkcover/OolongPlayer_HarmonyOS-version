# OPlayer（HarmonyOS 版）

OPlayer 是一个面向 HarmonyOS 的本地/网络音视频播放器示例，使用 **ArkTS + Native C++（NAPI）** 实现：

- ArkTS 负责界面与交互（文件选择、播放列表、进度条、全屏手势、音量/倍速/滤镜）。
- Native 层负责播放内核封装、OHAudio 音频输出和 EGL/GLES 视频上屏。

## 功能

- 打开本地音视频文件（DocumentViewPicker，按 fd 交给播放内核）与网络流（RTMP / HTTP / HTTPS）。
- 播放 / 暂停 / 停止，播放结束后自动切下一首。
- 进度条拖动、快进 / 快退（前沿立即生效 + 尾部去抖 + 串行化，SEEK_DONE 落点校正）。
- 0.5x ~ 2.0x 倍速，长按临时 2.0x，音量调节与静音。
- 画面滤镜：原色 / 灰度 / 反色 / 暖色 / 冷色。
- XComponent + EGL/GLES3 渲染 YUV420P，支持窗口尺寸变化后重绘最后一帧。
- 纯音频文件与纯音频容器自动隐藏视频层。

## 播放内核 SDK

本项目的播放能力来自独立的播放内核 **op-SDK**：

> 仓库地址：<https://github.com/Oolongmilkcover/op-SDK>

op-SDK 的特点：

- C 接口（`op_player.h`），便于 NAPI / JNI 等 FFI 调用。
- 音频为 **pull 模式**：壳在自己的音频线程里调用 `op_audio_read()` 拉 PCM。
- 视频帧为**借用视图**（`op_frame`），回调返回即失效，壳需当场消费。
- 支持本地文件（fd）与网络流、seek、倍速、逐帧等能力。

本仓库只保留 SDK 的**头文件**与**预编译动态库**：

- 头文件：`third_party/op-sdk/include/op/op_player.h`
- 动态库（随 HAP 打包）：`entry/libs/arm64-v8a/`、`entry/libs/x86_64/`
  （包含 `libop_sdk.so` 及其依赖的 FFmpeg 动态库、`libc++_shared.so`）

如需自行编译 SDK，请前往上面的 op-SDK 仓库，按目标 ABI 产出 `libop_sdk.so` 后替换 `entry/libs/<abi>/` 下的文件。

## 目录结构

```
entry/src/main/
├── ets/
│   ├── pages/Index.ets             # 播放器页面与全部 UI 交互
│   ├── model/PlayerController.ets  # 原生接口封装 + seek 状态机
│   └── entryability/               # UIAbility 入口
├── cpp/
│   ├── napi_init.cpp               # NAPI 导出、XComponent 表面回调
│   ├── player_core.cpp/.h          # op_player 生命周期、控制、事件转发
│   ├── audio_sink.cpp/.h           # OHAudio(AudioRenderer) 拉模式音频出口
│   └── video_renderer.cpp/.h       # XComponent + EGL/GLES3 YUV420P 渲染
└── resources/                      # 图标、布局等资源
AppScope/                           # 应用级配置
third_party/op-sdk/include/         # 播放内核头文件
```

## 关键实现说明

### 音频：OHAudio 写回调 + SDK 环形缓冲

SDK 是拉模式，而 HarmonyOS 的 `AudioRenderer` 是系统在写回调里要数据。`audio_sink.cpp` 在写回调中：

1. 从 SDK 拉 PCM（`op_audio_read`）；
2. 交回整块缓冲，不足部分补静音；
3. 在壳侧做音量缩放；
4. 把“设备里还没播出去的毫秒数”回报给 SDK（`op_audio_set_device_pending_ms`），用于音频时钟的延迟补偿。

为避免 seek 之后出现旧声与断续：

- seek 请求发出后先关闭音频门控（只出静音），`SEEK_DONE` 到达后再打开，避免旧位置的 PCM 被重新灌进设备；
- 落地后先预灌一小段数据（约 1.5 个写回调块，最长等 300ms）再连续出声，避免“半块数据 + 半块静音”反复拼接造成的卡顿；
- 门控/预灌都有超时兜底，`SEEK_DONE` 丢失也不会永久静音；
- `OH_AudioRenderer_Flush` 后重置帧计数基准，并在 flush 期间暂停设备积压上报，防止音频时钟被异常值带偏。

### seek 状态机

`PlayerController` 复刻了桌面版的控制策略：

- 连续按快进/快退：第一次立即 seek，后续只累积，静默 120ms 后补发最后一次；
- seek 串行化：上一次落地前不再重复投递；
- 进度条在等待落地期间钉在目标位置，`SEEK_DONE` 用真实落点校正；
- seek 期间过滤 SDK 的临时暂停事件，避免播放图标闪烁。

## 构建与运行

环境要求：

- DevEco Studio（HarmonyOS SDK，`compatibleSdkVersion` 6.1.1 / API 24，`targetSdkVersion` 26.0.0）
- 设备或模拟器：arm64-v8a（真机）或 x86_64（模拟器）

使用 DevEco Studio 打开本仓库，选择 `entry` 模块直接运行即可。

命令行构建（需先配置 `DEVECO_SDK_HOME` 指向 HarmonyOS SDK）：

```powershell
$env:DEVECO_SDK_HOME = 'D:\DevEcoStudio\DevEco Studio\sdk'
& "D:\DevEcoStudio\DevEco Studio\tools\hvigor\bin\hvigorw.bat" `
  --mode module -p product=default -p module=entry@default assembleHap --no-daemon
```

产物位于 `entry/build/default/outputs/default/`。当前 `build-profile.json5` 未配置签名，安装到真机前请在 DevEco Studio 中配置 `signingConfigs`。

## 说明

- 原生音频格式固定为 48000Hz / 双声道 / S16LE（`op_set_audio_format` 必须在 `op_open` 之前调用）。
- 网络流不支持快进/快退与逐帧；UI 会按 `op_is_stream()` 自动禁用相关按钮。
- `temp/` 目录为本地排查日志与参考实现，已在 `.gitignore` 中排除。
