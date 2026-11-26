# HarmonyOS / OpenHarmony x86_64（模拟器）交叉编译 toolchain。
#
# 与 op-SDK 自带的 cmake/toolchains/ohos-arm64.cmake 一一对应，只换 target 三元组。
# OHOS_NDK 指向 SDK 的 native 目录（含 llvm/bin 与 sysroot），例如：
#   D:/DevEcoStudio/DevEco Studio/sdk/default/openharmony/native
#
# SDK 自带的 clang 默认 target 是 Windows，所以 C/CXX/LD flags 都必须带
# --target=x86_64-linux-ohos，否则会按主机 ABI 编。
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT OHOS_NDK)
    if(DEFINED ENV{OHOS_NDK})
        set(OHOS_NDK "$ENV{OHOS_NDK}")
    else()
        message(FATAL_ERROR "请设置 OHOS_NDK（SDK 的 native 目录，含 llvm/bin 与 sysroot）")
    endif()
endif()

set(_OHOS_TC "${OHOS_NDK}/llvm/bin")
set(CMAKE_C_COMPILER   "${_OHOS_TC}/clang.exe")
set(CMAKE_CXX_COMPILER "${_OHOS_TC}/clang++.exe")
set(CMAKE_AR           "${_OHOS_TC}/llvm-ar.exe")
set(CMAKE_RANLIB       "${_OHOS_TC}/llvm-ranlib.exe")
set(CMAKE_STRIP        "${_OHOS_TC}/llvm-strip.exe")
set(CMAKE_NM           "${_OHOS_TC}/llvm-nm.exe")

set(CMAKE_SYSROOT "${OHOS_NDK}/sysroot")

set(CMAKE_C_FLAGS_INIT           "--target=x86_64-linux-ohos")
set(CMAKE_CXX_FLAGS_INIT         "--target=x86_64-linux-ohos")
set(CMAKE_EXE_LINKER_FLAGS_INIT  "--target=x86_64-linux-ohos -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--target=x86_64-linux-ohos -fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
