#include "player_core.h"

#include "audio_sink.h"
#include "video_renderer.h"

#include <hilog/log.h>

#include <cstring>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OPlayerCore"

namespace {

constexpr int32_t kSampleRate = 48000;
constexpr int32_t kChannels = 2;

struct EventPayload {
    int32_t type = 0;
    int32_t state = 0;
    int32_t code = 0;
    int64_t positionMs = 0;
    std::string message;
    int64_t durationMs = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t hasVideo = 0;
    int32_t hasAudio = 0;
    int32_t isStream = 0;
    int32_t isMusic = 0;
    int32_t isAudioMedia = 0;
};

void SetInt32(napi_env env, napi_value object, const char* name, int32_t value)
{
    napi_value v = nullptr;
    napi_create_int32(env, value, &v);
    napi_set_named_property(env, object, name, v);
}

void SetInt64(napi_env env, napi_value object, const char* name, int64_t value)
{
    napi_value v = nullptr;
    napi_create_int64(env, value, &v);
    napi_set_named_property(env, object, name, v);
}

void SetString(napi_env env, napi_value object, const char* name, const std::string& value)
{
    napi_value v = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &v);
    napi_set_named_property(env, object, name, v);
}

} // namespace

PlayerCore& PlayerCore::Get()
{
    static PlayerCore instance;
    return instance;
}

void PlayerCore::Init(napi_env env, napi_value eventCallback)
{
    Release();

    player_ = op_create();
    if (player_ == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "op_create failed");
        return;
    }

    op_set_event_cb(player_, OnEvent, this);
    op_set_video_cb(player_, OnVideo, this);
    op_set_log_cb(player_, OnLog, this);
    // 必须在 open 之前确定 PCM 格式
    op_set_audio_format(player_, kSampleRate, kChannels);
    AudioSink::Get().SetPlayer(player_);
    AudioSink::Get().SetVolume(0.5f);

    if (eventCallback != nullptr) {
        napi_value name = nullptr;
        napi_create_string_utf8(env, "OPlayerEvent", NAPI_AUTO_LENGTH, &name);
        napi_status status = napi_create_threadsafe_function(
            env, eventCallback, nullptr, name, 0, 1, nullptr, nullptr, nullptr, CallJs, &eventTsfn_);
        if (status != napi_ok) {
            eventTsfn_ = nullptr;
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "create threadsafe function failed");
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "player created, platform=%{public}d mobile=%{public}d",
                 op_platform(), op_is_mobile());
}

void PlayerCore::Release()
{
    // 先停音频：AudioRenderer 的写回调可能正在调 op_audio_read，
    // op_player 释放后再回调就是 use-after-free。
    AudioSink::Get().Close();

    if (player_ != nullptr) {
        op_stop(player_);
        op_destroy(player_);
        player_ = nullptr;
    }

    if (eventTsfn_ != nullptr) {
        napi_release_threadsafe_function(eventTsfn_, napi_tsfn_release);
        eventTsfn_ = nullptr;
    }
}

int32_t PlayerCore::Open(const std::string& path, int32_t fd, int64_t offset, int64_t size)
{
    if (player_ == nullptr) {
        return OP_ERR_NOT_READY;
    }

    op_open_options opt;
    std::memset(&opt, 0, sizeof(opt));
    opt.path = path.empty() ? nullptr : path.c_str();
    opt.fd = fd;
    opt.fd_offset = offset;
    opt.fd_size = size;
    opt.open_timeout_ms = 15000;

    const int32_t result = op_open(player_, &opt);
    if (result != OP_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "op_open failed: %{public}d", result);
        return result;
    }

    // 与 Qt 壳一致：音频设备在第一次打开时创建；换文件只清掉残留 PCM。
    AudioSink& audio = AudioSink::Get();
    // 新片源作废上一个文件遗留的 seek 门控，否则开头会被静音到超时。
    audio.EndSeek();
    if (!audio.IsOpen()) {
        audio.Open(player_, kSampleRate, kChannels);
    } else {
        audio.SetPlayer(player_);
        audio.Flush();
        audio.Start();
    }
    return result;
}

int32_t PlayerCore::Play()
{
    if (player_ == nullptr) return OP_ERR_NOT_READY;
    AudioSink::Get().Start();
    return op_play(player_);
}

int32_t PlayerCore::Pause()
{
    if (player_ == nullptr) return OP_ERR_NOT_READY;
    AudioSink::Get().Pause();
    return op_pause(player_);
}

int32_t PlayerCore::Stop()
{
    if (player_ == nullptr) return OP_ERR_NOT_READY;
    AudioSink& audio = AudioSink::Get();
    audio.Pause();
    audio.EndSeek();   // 取消可能悬着的 seek 门控，避免下次开播一直静音
    audio.Flush();
    return op_stop(player_);
}

int32_t PlayerCore::Seek(int64_t positionMs)
{
    if (player_ == nullptr) return OP_ERR_NOT_READY;
    // 先关门再清设备：写回调要么还在写旧数据（随后被 flush 清掉），要么从这里开始
    // 只出静音，直到 SEEK_DONE。否则旧位置的 PCM 会在 flush 之后又被灌进设备。
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "seek request %{public}lld ms",
                 (long long)positionMs);
    AudioSink& audio = AudioSink::Get();
    audio.BeginSeek();
    audio.Flush();
    const int32_t result = op_seek_ms(player_, positionMs);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, "seek submitted result=%{public}d", result);
    return result;
}

int32_t PlayerCore::SetSpeed(int32_t speedX10)
{
    if (player_ == nullptr) return OP_ERR_NOT_READY;
    return op_set_speed(player_, speedX10);
}

void PlayerCore::SetVolume(float volume)
{
    AudioSink::Get().SetVolume(volume);
}

void PlayerCore::SetFilter(int32_t type)
{
    VideoRenderer::Get().SetFilter(type);
}

void PlayerCore::OnEvent(void* user, const op_event* ev)
{
    auto* self = static_cast<PlayerCore*>(user);
    if (self == nullptr || ev == nullptr) {
        return;
    }

    // seek 落地：打开音频门控，让写回调先预灌一小段再出声
    if (ev->type == OP_EVENT_SEEK_DONE) {
        AudioSink::Get().EndSeek();
    }

    auto* payload = new EventPayload();
    payload->type = ev->type;
    payload->state = ev->state;
    payload->code = ev->code;
    payload->positionMs = ev->position_ms;
    if (ev->message != nullptr) {
        payload->message = ev->message;
    }

    if (ev->type == OP_EVENT_OPENED) {
        op_player* player = self->player_;
        payload->durationMs = op_duration_ms(player);
        payload->width = op_get_width(player);
        payload->height = op_get_height(player);
        payload->hasVideo = op_has_video(player);
        payload->hasAudio = op_has_audio(player);
        payload->isStream = op_is_stream(player);
        payload->isMusic = op_is_music(player);
        payload->isAudioMedia = op_is_audio_media(player);
    }

    if (self->eventTsfn_ != nullptr) {
        napi_status status = napi_call_threadsafe_function(self->eventTsfn_, payload, napi_tsfn_nonblocking);
        if (status != napi_ok) {
            delete payload;
        }
    } else {
        delete payload;
    }
}

void PlayerCore::OnVideo(void* user, const op_frame* frame)
{
    (void)user;
    // op_frame 是借用视图：必须在回调里用完，不能存指针。
    VideoRenderer::Get().Render(frame);
}

void PlayerCore::OnLog(void* user, int32_t level, const char* line)
{
    (void)user;
    if (line == nullptr) return;
    LogLevel logLevel = LOG_INFO;
    if (level == 0) logLevel = LOG_DEBUG;
    else if (level == 2) logLevel = LOG_WARN;
    else if (level == 3) logLevel = LOG_ERROR;
    OH_LOG_Print(LOG_APP, logLevel, LOG_DOMAIN, "OPlayerSDK", "%{public}s", line);
}

void PlayerCore::CallJs(napi_env env, napi_value jsCallback, void* context, void* data)
{
    (void)context;
    auto* payload = static_cast<EventPayload*>(data);
    if (payload == nullptr) {
        return;
    }

    if (env == nullptr || jsCallback == nullptr) {
        delete payload;
        return;
    }

    napi_handle_scope scope = nullptr;
    napi_open_handle_scope(env, &scope);

    napi_value object = nullptr;
    napi_create_object(env, &object);
    SetInt32(env, object, "type", payload->type);
    SetInt32(env, object, "state", payload->state);
    SetInt32(env, object, "code", payload->code);
    SetInt64(env, object, "positionMs", payload->positionMs);
    SetString(env, object, "message", payload->message);
    SetInt64(env, object, "durationMs", payload->durationMs);
    SetInt32(env, object, "width", payload->width);
    SetInt32(env, object, "height", payload->height);
    SetInt32(env, object, "hasVideo", payload->hasVideo);
    SetInt32(env, object, "hasAudio", payload->hasAudio);
    SetInt32(env, object, "isStream", payload->isStream);
    SetInt32(env, object, "isMusic", payload->isMusic);
    SetInt32(env, object, "isAudioMedia", payload->isAudioMedia);

    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value result = nullptr;
    napi_call_function(env, global, jsCallback, 1, &object, &result);

    napi_close_handle_scope(env, scope);
    delete payload;
}
