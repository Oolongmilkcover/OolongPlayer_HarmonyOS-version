# OPlayer —— 鸿蒙播放器原生层
# 结构：
#   napi_init.cpp      NAPI 导出 + XComponent 表面回调
#   player_core.*      op_sdk 生命周期/播放控制/事件转发
#   audio_sink.*       OHAudio(AudioRenderer) 拉模式音频出口（含音量缩放）
#   video_renderer.*   XComponent + EGL/GLES3 的 YUV420P 渲染（含滤镜）
cmake_minimum_required(VERSION 3.5.0)
project(OPlayer)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# op-sdk 发行包头文件（third_party/op-sdk/include/op/op_player.h）
set(OP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/op-sdk)
# 预编译动态库随 HAP 打包：entry/libs/<abi>/
set(OP_SDK_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH})

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${OP_SDK_ROOT}/include)

add_library(entry SHARED
        napi_init.cpp
        player_core.cpp
        audio_sink.cpp
        video_renderer.cpp)

target_link_libraries(entry PUBLIC
        libace_napi.z.so
        libace_ndk.z.so
        libnative_window.so
        libohaudio.so
        libEGL.so
        libGLESv3.so
        libhilog_ndk.z.so)

# 播放器 SDK（自带 FFmpeg 依赖，运行时从 HAP 的 libs 目录解析）
target_link_libraries(entry PUBLIC ${OP_SDK_LIB_DIR}/libop_sdk.so)
set_target_properties(entry PROPERTIES BUILD_RPATH "$ORIGIN")
