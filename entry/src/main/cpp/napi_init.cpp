#include "player_core.h"
#include "video_renderer.h"

#include <napi/native_api.h>
#include <native_window/external_window.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool GetStringArg(napi_env env, napi_value value, std::string& out)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
        return false;
    }
    std::vector<char> buffer(length + 1, 0);
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &copied) != napi_ok) {
        return false;
    }
    out.assign(buffer.data(), copied);
    return true;
}

napi_value Int32Result(napi_env env, int32_t value)
{
    napi_value result = nullptr;
    napi_create_int32(env, value, &result);
    return result;
}

napi_value Int64Result(napi_env env, int64_t value)
{
    napi_value result = nullptr;
    napi_create_int64(env, value, &result);
    return result;
}

napi_value BoolResult(napi_env env, bool value)
{
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

/* ---------------------------------------------------------------- 视频表面 */

// attachSurface(surfaceId: string, width: number, height: number): boolean
napi_value JsAttachSurface(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::string surfaceIdText;
    if (argc < 1 || !GetStringArg(env, args[0], surfaceIdText)) {
        return BoolResult(env, false);
    }
    uint64_t surfaceId = std::strtoull(surfaceIdText.c_str(), nullptr, 10);

    int32_t width = 0;
    int32_t height = 0;
    if (argc >= 2 && args[1] != nullptr) napi_get_value_int32(env, args[1], &width);
    if (argc >= 3 && args[2] != nullptr) napi_get_value_int32(env, args[2], &height);

    OHNativeWindow* window = nullptr;
    int32_t ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (ret != 0 || window == nullptr) {
        return BoolResult(env, false);
    }

    VideoRenderer::Get().SetSurface(window, width, height);
    return BoolResult(env, true);
}

napi_value JsResizeSurface(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t width = 0;
    int32_t height = 0;
    if (argc >= 1 && args[0] != nullptr) napi_get_value_int32(env, args[0], &width);
    if (argc >= 2 && args[1] != nullptr) napi_get_value_int32(env, args[1], &height);
    VideoRenderer::Get().ResizeSurface(width, height);
    return nullptr;
}

napi_value JsRefreshSurface(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t width = 0;
    int32_t height = 0;
    if (argc >= 1 && args[0] != nullptr) napi_get_value_int32(env, args[0], &width);
    if (argc >= 2 && args[1] != nullptr) napi_get_value_int32(env, args[1], &height);
    VideoRenderer::Get().Refresh(width, height);
    return nullptr;
}

napi_value JsDetachSurface(napi_env env, napi_callback_info info)
{
    (void)env;
    (void)info;
    VideoRenderer::Get().ClearSurface();
    return nullptr;
}

op_player* CurrentPlayer()
{
    return PlayerCore::Get().Player();
}

/* ---------------------------------------------------------------- 生命周期 */

napi_value JsInit(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    napi_valuetype type = napi_undefined;
    if (argc >= 1) {
        napi_typeof(env, args[0], &type);
    }
    if (type == napi_function) {
        PlayerCore::Get().Init(env, args[0]);
    } else {
        PlayerCore::Get().Init(env, nullptr);
    }
    return nullptr;
}

napi_value JsDestroy(napi_env env, napi_callback_info info)
{
    (void)info;
    PlayerCore::Get().Release();
    return nullptr;
}

// open(path: string, fd: number, offset: number, size: number): number
napi_value JsOpen(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::string path;
    if (argc >= 1) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, args[0], &type);
        if (type == napi_string) {
            GetStringArg(env, args[0], path);
        }
    }

    int32_t fd = -1;
    int64_t offset = 0;
    int64_t size = 0;
    if (argc >= 2 && args[1] != nullptr) {
        napi_get_value_int32(env, args[1], &fd);
    }
    if (argc >= 3 && args[2] != nullptr) {
        napi_get_value_int64(env, args[2], &offset);
    }
    if (argc >= 4 && args[3] != nullptr) {
        napi_get_value_int64(env, args[3], &size);
    }

    return Int32Result(env, PlayerCore::Get().Open(path, fd, offset, size));
}

/* ---------------------------------------------------------------- 播放控制 */

napi_value JsPlay(napi_env env, napi_callback_info info)
{
    (void)info;
    return Int32Result(env, PlayerCore::Get().Play());
}

napi_value JsPause(napi_env env, napi_callback_info info)
{
    (void)info;
    return Int32Result(env, PlayerCore::Get().Pause());
}

napi_value JsStop(napi_env env, napi_callback_info info)
{
    (void)info;
    return Int32Result(env, PlayerCore::Get().Stop());
}

napi_value JsSeek(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int64_t position = 0;
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_int64(env, args[0], &position);
    }
    return Int32Result(env, PlayerCore::Get().Seek(position));
}

napi_value JsSetSpeed(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t speedX10 = 10;
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &speedX10);
    }
    return Int32Result(env, PlayerCore::Get().SetSpeed(speedX10));
}

napi_value JsSetVolume(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    double volume = 0.5;
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_double(env, args[0], &volume);
    }
    PlayerCore::Get().SetVolume((float)volume);
    return nullptr;
}

napi_value JsSetFilter(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t type = 0;
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &type);
    }
    PlayerCore::Get().SetFilter(type);
    return nullptr;
}

/* ---------------------------------------------------------------- 查询 */

#define OP_QUERY_INT32(jsName, apiCall)                          \
    napi_value jsName(napi_env env, napi_callback_info info)     \
    {                                                            \
        (void)info;                                              \
        op_player* player = CurrentPlayer();                     \
        return Int32Result(env, player ? apiCall : 0);           \
    }

#define OP_QUERY_INT64(jsName, apiCall)                          \
    napi_value jsName(napi_env env, napi_callback_info info)     \
    {                                                            \
        (void)info;                                              \
        op_player* player = CurrentPlayer();                     \
        return Int64Result(env, player ? apiCall : 0);           \
    }

OP_QUERY_INT64(JsGetDuration, op_duration_ms(player))
OP_QUERY_INT64(JsGetPosition, op_position_ms(player))
OP_QUERY_INT32(JsGetWidth, op_get_width(player))
OP_QUERY_INT32(JsGetHeight, op_get_height(player))
OP_QUERY_INT32(JsGetState, op_state_of(player))
OP_QUERY_INT32(JsHasVideo, op_has_video(player))
OP_QUERY_INT32(JsHasAudio, op_has_audio(player))
OP_QUERY_INT32(JsIsStream, op_is_stream(player))
OP_QUERY_INT32(JsIsMusic, op_is_music(player))
OP_QUERY_INT32(JsIsAudioMedia, op_is_audio_media(player))
OP_QUERY_INT32(JsGetSpeedX10, op_get_speedX10(player))
OP_QUERY_INT32(JsIsMobile, op_is_mobile())

#undef OP_QUERY_INT32
#undef OP_QUERY_INT64

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"init", nullptr, JsInit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroy", nullptr, JsDestroy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachSurface", nullptr, JsAttachSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeSurface", nullptr, JsResizeSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"refreshSurface", nullptr, JsRefreshSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"detachSurface", nullptr, JsDetachSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"open", nullptr, JsOpen, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"play", nullptr, JsPlay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pause", nullptr, JsPause, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, JsStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"seek", nullptr, JsSeek, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSpeed", nullptr, JsSetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVolume", nullptr, JsSetVolume, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFilter", nullptr, JsSetFilter, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDuration", nullptr, JsGetDuration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPosition", nullptr, JsGetPosition, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getWidth", nullptr, JsGetWidth, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getHeight", nullptr, JsGetHeight, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getState", nullptr, JsGetState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hasVideo", nullptr, JsHasVideo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hasAudio", nullptr, JsHasAudio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isStream", nullptr, JsIsStream, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isMusic", nullptr, JsIsMusic, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isAudioMedia", nullptr, JsIsAudioMedia, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSpeedX10", nullptr, JsGetSpeedX10, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isMobile", nullptr, JsIsMobile, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module opPlayerModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&opPlayerModule);
}
