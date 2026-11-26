# OPlayer（HarmonyOS 版）

OPlayer 是一个面向 HarmonyOS 的本地/网络音视频播放器示例，使用 **ArkTS + Native C++（NAPI）** 实现：

- ArkTS 负责界面与交互（文件选择、播放列表、进度条、全屏手势、倍速/滤镜）。
- Native 层负责播放内核封装、OHAudio 音频输出和 EGL/GLES 视频上屏。

## 功能

- 打开本地音视频文件（DocumentViewPicker，按 fd 交给播放内核）与网络流（RTMP / HTTP / HTTPS）。
- 播放 / 暂停 / 停止，播放结束后自动切下一首。
- 进度条拖动、快进 / 快退（前沿立即生效 + 尾部去抖 + 串行化，SEEK_DONE 落点校正）。
- 0.5x ~ 2.0x 倍速，长按临时 2.0x。
- 音量固定为 1.0（不做软件增益），由手机系统音量键统一调节。
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

## 鸿蒙侧如何使用 op-SDK 的 C API

本项目的调用链路如下，下文按这条链路说明 C API 在鸿蒙工程中的实际用法：

```
ArkTS（Index.ets / PlayerController.ets）
  └─ NAPI（napi_init.cpp，暴露 open/play/seek 等方法）
       └─ PlayerCore（player_core.cpp） ←→ op_player C API
            ├─ AudioSink（audio_sink.cpp）        → OHAudio AudioRenderer
            └─ VideoRenderer（video_renderer.cpp）→ XComponent + EGL/GLES
```

### 1. 工程内链接

`entry/src/main/cpp/CMakeLists.txt` 中把 SDK 头文件目录加入 include，并链到
`libs/<abi>/libop_sdk.so`；运行时该目录下的 FFmpeg 与 `libc++_shared.so` 也要一起进 HAP：

```cmake
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/op-sdk/include)

target_link_libraries(entry PUBLIC
        libohaudio.so        # 音频输出
        libnative_window.so  # XComponent 窗口
        libEGL.so
        libGLESv3.so
        libhilog_ndk.z.so
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}/libop_sdk.so)

set_target_properties(entry PROPERTIES BUILD_RPATH "$ORIGIN")
```

### 2. 创建实例并注册回调

`PlayerCore::Init()` 里创建 `op_player`。音频格式必须在 `op_open` 之前确定，
SDK 会据此配置整条音频链路：

```cpp
// player_core.cpp
int32_t PlayerCore::Init()
{
    player_ = op_create();
    if (player_ == nullptr) return OP_ERR_NO_MEMORY;

    op_set_event_cb(player_, &PlayerCore::OnEvent, this);  // 状态/进度/seek 结果
    op_set_video_cb(player_, &PlayerCore::OnVideo, this);  // 每解码一帧回调一次
    op_set_audio_format(player_, 48000, 2);                // 48kHz / 双声道 / S16

    AudioSink::Get().SetPlayer(player_);
    return OP_OK;
}
```

回调由 SDK 内部线程触发，鸿蒙侧不能在这些线程直接碰 UI：

- 事件：拷出 `op_event` 的数值，经 `napi_call_threadsafe_function` 投递到 JS 线程；
- 视频：在回调线程内直接交给 `VideoRenderer` 上传 GL 纹理（借用视图，返回即失效）；
- 音频：由 OHAudio 的写回调线程主动来拉，见第 6 节。

### 3. 打开片源（path / fd）

`JsOpen` 把 ArkTS 传来的路径或 picker fd 原样交给 SDK：

```cpp
// player_core.cpp
int32_t PlayerCore::Open(const std::string& name, int32_t fd)
{
    op_open_options opt{};
    opt.path = name.c_str();   // 本地路径或 http:// / rtmp:// 等 URL
    opt.fd = fd;               // 用 picker 时填这里，path 仅用于扩展名提示
    opt.fd_offset = 0;
    opt.fd_size = 0;           // 0 = 自动探测
    opt.open_timeout_ms = 15000;
    return op_open(player_, &opt);
}
```

鸿蒙侧的三条 fd 约束（`Index.ets` 已按此实现）：

1. fd 归 ArkTS 的 `fs.File` 所有，SDK **不会 close**；
2. 播放期间必须保持打开，SDK 的 demux 线程全程在读；
3. `op_open` / `op_stop` 都是**异步投递**，请求返回时旧片源可能还在收尾。
   因此切歌/停止/退出时不能立刻 `fs.closeSync`，要等新片源的 `OP_EVENT_OPENED`
   或 `OP_STATE_STOPPED` 事件回来后再关，否则会出现 EBADF、花屏或切歌失败。

### 4. 事件回调：从 SDK 线程转到 JS 线程

```cpp
// player_core.cpp
void PlayerCore::OnEvent(void* user, const op_event* ev)
{
    if (ev == nullptr) return;
    auto* self = static_cast<PlayerCore*>(user);

    auto* payload = new EventPayload();
    payload->type       = ev->type;
    payload->state      = ev->state;
    payload->code       = ev->code;
    payload->positionMs = ev->position_ms;   // POSITION / SEEK_DONE / COMPLETED

    // 文件级属性只在 OPENED 时查一次
    if (ev->type == OP_EVENT_OPENED) {
        payload->durationMs = op_duration_ms(self->player_);
        payload->width      = op_get_width(self->player_);
        payload->height     = op_get_height(self->player_);
        payload->hasVideo   = op_has_video(self->player_);
        payload->hasAudio   = op_has_audio(self->player_);
        payload->isStream   = op_is_stream(self->player_);
    }

    if (napi_call_threadsafe_function(self->tsfn_, payload, napi_tsfn_nonblocking) != napi_ok) {
        delete payload;   // 投递失败要自己回收
    }
}
```

`ev->message` 只在回调期间有效，需要保留就当场拷贝。回调里**不要**调用
`op_stop` / `op_destroy`，否则会自己等自己造成死锁。

### 5. 视频帧：交给 XComponent 渲染

```cpp
// player_core.cpp
void PlayerCore::OnVideo(void* user, const op_frame* frame)
{
    VideoRenderer::Get().Render(frame);   // 回调内同步消费
}
```

`VideoRenderer` 在 `XComponent` 的 surface 上创建 EGL/GLES3 环境，把 YUV420P
三平面上传成纹理再绘制：

```cpp
const uint8_t* y = frame->data;
const uint8_t* u = y + frame->linesize[0] * frame->height;
const uint8_t* v = u + frame->linesize[1] * (frame->height / 2);
```

`frame` 与 `frame->data` 都是借用视图：不能保存指针、不能 `free`，回调返回后即失效。
窗口尺寸变化（转屏/全屏）时通过 `resizeSurface` / `refreshSurface` 重建 EGLSurface
并重绘最后一帧。

### 6. 音频：用 OHAudio 写回调适配 pull 模式

`AudioSink` 创建一个 48kHz/双声道/S16 的 `OH_AudioRenderer`，在系统写回调里向
SDK 拉数据：

```cpp
// audio_sink.cpp
static OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* renderer, void* user,
                                                void* audioData, int32_t audioDataSize)
{
    auto* self = static_cast<AudioSink*>(user);
    int32_t got = 0;

    // seek 未落地 / 刚 flush 时先只出静音，避免旧 PCM 回灌和半块数据拼接
    if (self->CanOutput()) {
        got = op_audio_read(self->player_, audioData, audioDataSize);
    }
    if (got < audioDataSize) {
        memset(static_cast<uint8_t*>(audioData) + got, 0, audioDataSize - got);
    }

    // 音量不做软件增益（恒为 1.0），直接交给系统音量控制

    // 回报设备里已写入但还没播出的毫秒数，供 SDK 做音频时钟延迟补偿
    const int32_t pendingMs = self->QueryDevicePendingMs(renderer);
    op_audio_set_device_pending_ms(self->player_, pendingMs);

    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}
```

鸿蒙侧要点：

- 回调必须交回**整块**缓冲，不足部分补静音，否则设备会反复欠载、爆音；
- `op_audio_read` 返回 0 表示暂时没有数据，直接补静音即可，不要忙转；
- `OH_AudioRenderer_Flush` 会让时间戳的 `framePosition` 归零，而 `GetFramesWritten`
  仍是累计值，必须记住 flush 时的基准再算设备积压，否则音画会突然不同步；
- 暂停时写回调不会触发，不需要回报设备延迟。

### 7. 播放控制与 seek

```cpp
op_play(player_);
op_pause(player_);
op_stop(player_);            // 回到 IDLE，player_ 可复用
op_set_speed(player_, 15);   // ×10，15 = 1.5x
op_seek_ms(player_, ms);     // 异步，返回 OP_OK 只代表已受理
```

鸿蒙侧 seek 的完整动作：

```cpp
// player_core.cpp
AudioSink::Get().BeginSeek();   // 关音频门控：SEEK_DONE 前只出静音
AudioSink::Get().Flush();       // 丢掉设备里还没播的旧 PCM
op_seek_ms(player_, positionMs);

// 收到 OP_EVENT_SEEK_DONE 后
AudioSink::Get().EndSeek();     // 解门控，并预灌约 1.5 个回调块再连续出声
```

JS 层的 `PlayerController` 负责：上一次 `SEEK_DONE` 前不重复投递（串行化）、
连续快进/快退的尾部去抖、等待落地期间把进度条钉在目标位置、用真实的
`SEEK_DONE` 位置校正。`OP_EVENT_POSITION` 的 `position_ms` 只用于刷新进度。

### 8. 释放顺序

```cpp
// player_core.cpp
int32_t PlayerCore::Release()
{
    AudioSink::Get().Close();   // 先停并释放 AudioRenderer，音频线程不会再进来
    if (player_ != nullptr) {
        op_stop(player_);
        op_destroy(player_);    // join 所有内部线程，返回后不再有回调
        player_ = nullptr;
    }
    return OP_OK;
}
```

必须**先停音频输出、再 `op_destroy`**；否则音频线程可能正在 `op_audio_read`，
`op_player` 释放后就是 use-after-free。随后 `JsDestroy` 再释放事件用的 TSFN。

### 9. 鸿蒙侧常见坑

| 现象 | 常见原因 |
| --- | --- |
| 切歌偶发失败、花屏 | 新片源 OPENED 之前就把旧 fd `close` 了 |
| seek 后听到旧位置的声音 | flush 之后、seek 落地之前又读了旧 PCM |
| seek 后声音断续 | 环形缓冲只有零碎数据，半块数据 + 半块静音反复拼接 |
| 音画不同步 | 没回报设备积压，或 flush 后帧基准没重置 |
| 回调线程卡死 | 在回调里调了 `op_stop` / `op_destroy`，或做了阻塞操作 |
| 日志报 Illegal state | 对已运行的 `AudioRenderer` 重复 `Start`，加状态判断即可 |

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
3. 不做软件音量缩放（恒为 1.0），音量交给手机系统统一控制；
4. 把“设备里还没播出去的毫秒数”回报给 SDK（`op_audio_set_device_pending_ms`），用于音频时钟的延迟补偿。

为避免 seek 之后出现旧声与断续：

- seek 请求发出后先关闭音频门控（只出静音），`SEEK_DONE` 到达后再打开，避免旧位置的 PCM 被重新灌进设备；
- 落地后先预灌一小段数据（约 1.5 个写回调块，最长等 300ms）再连续出声，避免“半块数据 + 半块静音”反复拼接造成的卡顿；
- 门控/预灌都有超时兜底，`SEEK_DONE` 丢失也不会永久静音；
- `OH_AudioRenderer_Flush` 后重置帧计数基准，并在 flush 期间暂停设备积压上报，防止音频时钟被异常值带偏。

### seek 状态机

`PlayerController` 复刻了成熟播放器的控制策略：

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
