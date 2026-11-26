#pragma once

#include <op/op_player.h>

#include <napi/native_api.h>

#include <atomic>
#include <string>

/*
 * 播放器原生单例：持有 op_player，转发 SDK 事件到 JS（threadsafe function），
 * 视频帧直接交给 VideoRenderer 上屏，音频由 AudioSink 拉流。
 *
 * 回调线程：op_event_cb / op_video_cb 跑在 SDK 自己的线程上，
 * 这里不做 UI 操作——事件经 napi_threadsafe_function 回到 JS 线程，
 * 视频帧在回调内同步渲染（op_frame 是借用视图，回调返回即失效）。
 */
class PlayerCore {
public:
    static PlayerCore& Get();

    void Init(napi_env env, napi_value eventCallback);
    void Release();

    int32_t Open(const std::string& path, int32_t fd, int64_t offset, int64_t size);
    int32_t Play();
    int32_t Pause();
    int32_t Stop();
    int32_t Seek(int64_t positionMs);
    int32_t SetSpeed(int32_t speedX10);
    void SetVolume(float volume);
    void SetFilter(int32_t type);

    bool HasPlayer() const { return player_ != nullptr; }
    op_player* Player() const { return player_; }

private:
    PlayerCore() = default;
    ~PlayerCore() = default;
    PlayerCore(const PlayerCore&) = delete;
    PlayerCore& operator=(const PlayerCore&) = delete;

    static void OnEvent(void* user, const op_event* ev);
    static void OnVideo(void* user, const op_frame* frame);
    static void OnLog(void* user, int32_t level, const char* line);
    static void CallJs(napi_env env, napi_value jsCallback, void* context, void* data);

    op_player* player_ = nullptr;
    napi_threadsafe_function eventTsfn_ = nullptr;
};
