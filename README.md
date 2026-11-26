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

## 如何在你的项目里接入 op-SDK 的 C API

下面按"从零接一个播放器"的顺序给出最小可用写法。完整实现可对照本仓库的
`entry/src/main/cpp/player_core.cpp`、`audio_sink.cpp`、`video_renderer.cpp`。

### 1. 引入头文件并链接动态库

CMake 里只需要头文件目录和 `libop_sdk.so`：

```cmake
# third_party/op-sdk/include/op/op_player.h
include_directories(${OP_SDK_ROOT}/include)

add_library(entry SHARED your_napi.cpp player_core.cpp audio_sink.cpp video_renderer.cpp)

target_link_libraries(entry PUBLIC
        libohaudio.so       # 音频输出
        libnative_window.so # 视频上屏
        libEGL.so
        libGLESv3.so
        libhilog_ndk.z.so
        ${OP_SDK_LIB_DIR}/libop_sdk.so)

# .so 在运行时从 HAP 的 libs/<abi>/ 解析（依赖的 FFmpeg 也要一起放进去）
set_target_properties(entry PROPERTIES BUILD_RPATH "$ORIGIN")
```

`libop_sdk.so` 依赖 FFmpeg 与 `libc++_shared.so`，运行时必须和它放在同一目录
（本仓库是 `entry/libs/<abi>/`，由 Hvigor 自动打包进 HAP）。

### 2. 创建实例、注册回调、确定音频格式

```cpp
#include <op/op_player.h>

op_player* player = op_create();
if (player == nullptr) { /* 错误处理 */ }

op_set_event_cb(player, OnEvent, this);   // 状态/进度/seek 结果
op_set_video_cb(player, OnVideo, this);   // 每解码一帧回调一次
op_set_log_cb(player, OnLog, this);       // 可选：SDK 日志

// 必须在 op_open 之前！open 时整条音频链路会按这组参数配置好
op_set_audio_format(player, 48000, 2);    // 48kHz / 双声道
```

回调都在 SDK 自己的线程上触发，**不要在里面做 UI 操作或阻塞等待**。本仓库用
`napi_create_threadsafe_function` 把事件投递回 JS 线程；Qt 壳则用
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`。思路一样：把
`op_event` 里的数值**当场拷走**，队列化后再处理。

### 3. 打开片源（path 或 fd）

```cpp
op_open_options opt{};
opt.path = "movie.mp4";   // 本地路径或 rtsp:// http:// 等 URL
opt.fd = -1;              // 用 picker 的 fd 时填这里，path 只作扩展名提示
opt.fd_offset = 0;
opt.fd_size = 0;          // 0 = 自动探测
opt.open_timeout_ms = 15000;

int32_t ret = op_open(player, &opt);
if (ret != OP_OK) { /* 打不开：看返回值 */ }
```

使用 fd 时有三条硬约束（本仓库踩过坑）：

1. fd 归调用方所有，SDK **不会 close** 它；
2. **播放期间必须一直开着**，demux 线程全程在读；
3. `op_open` / `op_stop` 都是**异步投递**，不要一调用就立刻关闭旧 fd。
   正确做法是等新片源的 `OP_EVENT_OPENED`（或停止后的 `OP_EVENT_STATE == OP_STATE_STOPPED`）
   再把旧 fd 关掉，否则会出现 EBADF、花屏或切歌失败。

### 4. 事件回调：把"点事件"的数值拷走

```cpp
void OnEvent(void* user, const op_event* ev)
{
    if (ev == nullptr) return;

    auto* payload = new EventPayload();
    payload->type       = ev->type;
    payload->state      = ev->state;
    payload->code       = ev->code;
    payload->positionMs = ev->position_ms;   // POSITION / SEEK_DONE / COMPLETED 用它

    // OPENED 时才去查文件级属性（一次打开期间不变）
    if (ev->type == OP_EVENT_OPENED) {
        op_player* p = static_cast<PlayerCore*>(user)->Player();
        payload->durationMs = op_duration_ms(p);
        payload->width      = op_get_width(p);
        payload->height     = op_get_height(p);
        payload->hasVideo   = op_has_video(p);
        payload->hasAudio   = op_has_audio(p);
        payload->isStream   = op_is_stream(p);
    }

    // 投递到 UI 线程；nonblocking 失败时要自己 delete
    if (napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
        delete payload;
    }
}
```

要点：

- `ev->message` 只在回调期间有效，要留就当场拷贝；
- **回调里不要调用 `op_destroy` / `op_stop`**，那是自己等自己，会死锁；
- 事件回调线程不保证与视频/音频回调是同一根线程。

### 5. 视频帧：借用视图，回调返回即失效

```cpp
void OnVideo(void* user, const op_frame* frame)
{
    // 本仓库在回调里直接上传 GL 纹理（同步消费）
    // YUV420P 三平面偏移：
    const uint8_t* y = frame->data;
    const uint8_t* u = y + frame->linesize[0] * frame->height;
    const uint8_t* v = u + frame->linesize[1] * (frame->height / 2);

    // 若是 Qt/JS 这类必须跨线程交付的场景：
    //   op_frame* copy = op_frame_clone(frame);
    //   投递到目标线程，用完 op_frame_free(&copy);
}
```

**绝对不要** `free(frame->data)`，也不要保存 `frame` 指针。要跨线程就用
`op_frame_clone()` 深拷贝。

### 6. 音频：pull 模式适配平台播放器

SDK 不主动推送 PCM，而是由壳在自己的音频线程里拉：

```cpp
// OHAudio 的写回调（系统在音频线程上要一整块数据）
static OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* renderer, void* user,
                                                void* audioData, int32_t audioDataSize)
{
    int32_t got = 0;
    if (player != nullptr && !paused) {
        got = op_audio_read(player, audioData, audioDataSize);  // 返回 0 = 暂时没数据
    }

    // 回调必须交回整块缓冲；不足部分补静音，否则设备会反复欠载/爆音
    if (got < audioDataSize) {
        memset((uint8_t*)audioData + got, 0, audioDataSize - got);
    }

    // 自己处理音量：SDK 不管音量，这里对 S16 样本做缩放
    ApplyVolume(audioData, got);

    // 把"设备里已经写入但还没播出去"的毫秒数回报给 SDK（音频时钟延迟补偿）
    int32_t pendingMs = QueryDevicePendingMs(renderer);
    op_audio_set_device_pending_ms(player, pendingMs);

    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}
```

播放控制：

```cpp
op_play(player);
op_pause(player);
op_stop(player);          // 回到 IDLE，p 仍可复用
op_set_speed(player, 15); // ×10，15 = 1.5x（0.5x ~ 2.0x）
```

音频接入的四个经验点：

1. **不要忙转**：`op_audio_read` 返回 0 表示暂时没数据，睡 2~5ms 再试；
2. **seek 时先关门再 flush**：seek 请求发出后到 `SEEK_DONE` 之间不要继续把旧 PCM 写进设备，
   否则会听到 seek 前的旧声音；落地后再预灌一小段（约 1.5 个回调块）再连续出声，
   避免"半块数据 + 半块静音"造成断续；
3. **flush 后重置帧基准**：`OH_AudioRenderer_Flush` 会让时间戳的 `framePosition` 归零，
   而 `GetFramesWritten` 仍是累计值，直接相减会得到巨大积压，必须记住 flush 时的基准；
4. **暂停时不用回报设备延迟**，SDK 会冻结外推。

### 7. seek：异步请求 + 落地校正

```cpp
AudioSink::Get().BeginSeek();   // 可选：先关音频门控
AudioSink::Get().Flush();       // 丢掉设备里还没播的旧 PCM
op_seek_ms(player, 60 * 1000);

// ...之后会收到 OP_EVENT_SEEK_DONE，position_ms 是实际落点（可能对齐关键帧）
```

`op_seek_ms` 是异步的，返回 `OP_OK` 只代表"已受理"。UI 层建议：

- 对 seek 做**串行化**：上一次 `SEEK_DONE` 之前不再重复投递；
- 连续快进/快退做**尾部去抖**：前沿立即生效，后续累积到静默后再补发；
- `SEEK_DONE` 的落点可能和请求值不同，用它校正进度条；
- 不要用"暂停 + seek + 恢复播放"实现，SDK 内部会处理播放状态。

### 8. 释放

```cpp
op_destroy(player);   // 会 join 所有内部线程，返回后不会再有任何回调
```

顺序必须是：**先停音频输出（AudioRenderer / AudioTrack）、再 `op_destroy`**。
否则音频线程可能正在 `op_audio_read`，`op_player` 释放后就是一个 use-after-free。

### 9. 常见坑速查

| 现象 | 常见原因 |
| --- | --- |
| 打开/切歌偶发失败、花屏 | fd 在新片源 OPENED 之前就被 close 了 |
| seek 后听到旧位置的声音 | flush 之后、seek 落地之前又读了旧 PCM |
| seek 后声音断续 | 环形缓冲只有零碎数据，半块数据 + 半块静音反复拼接 |
| 音画不同步 | 没有回报设备积压（`op_audio_set_device_pending_ms`），或 flush 后帧基准没重置 |
| 回调里偶发卡死 | 在回调里调了 `op_stop` / `op_destroy`，或做了阻塞操作 |
| 日志里 start/renderer 报 Illegal state | 对已经运行的 AudioRenderer 重复 `Start`，加状态判断即可 |

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
