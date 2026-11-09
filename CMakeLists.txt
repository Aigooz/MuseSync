#==============================================================================
# CMakeLists.txt — 跨平台构建 MuseTone
#
# macOS 本地开发仍可用 ./build.sh（含 6 项验证）；
# 本文件用于 Windows（MSVC）与 Linux（GCC + X11）构建，也能构建 macOS 版。
#
# 用法（Windows）：
#   git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# 用法（Linux）：
#   sudo apt install libx11-dev libxft-dev      # Ubuntu 20.04 上会自动带上 libfreetype6-dev
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVST3_SDK_DIR=./vst3sdk
#   cmake --build build --parallel
#
# ⚠ 构建底座的 glibc 版本 = 产物要求的 glibc 下限。
#   官方 CI 在 **Ubuntu 20.04**（glibc 2.31 / GCC 9）里构建，产物可跑在
#   Ubuntu 20.04+ / Debian 11+ / RHEL 9+。
#   若在更新的发行版上构建，产物的 GLIBC_* 符号需求会被顶高：
#   例如 Ubuntu 24.04（glibc 2.39 + GCC 13）会因 -std=gnu++17 隐含 _GNU_SOURCE
#   → 打开 _ISOC23_SOURCE → glibc 把 sscanf 宏重定向到 __isoc23_sscanf
#   → 产物硬性要求 GLIBC_2.38，Ubuntu 22.04 / Debian 12 直接加载失败。
#   CI 里有断言守住这一条（见 .github/workflows/build-linux.yml）。
#
# 产物：Windows build/**/MuseSync.vst3；其他平台 build/**/MuseTone.vst3
#==============================================================================
cmake_minimum_required(VERSION 3.20)

# Keep the build target and bundle path stable for existing host references.
project(MuseTone VERSION 2.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MUSETONE_BUILD_TESTS "Build the focused MuseTone regression tests" OFF)

# 可选的 Windows 视频播放器依赖。未提供 SDK 时仍保留无功能降级实现，
# 这样普通构建不会因为本机没有 WebView2 SDK 而失败。
set(WEBVIEW2_SDK_DIR "" CACHE PATH
    "WebView2 SDK 根目录（含 build/native/include/WebView2.h）")

# 单配置生成器（Unix Makefiles / Ninja）必须显式给 CMAKE_BUILD_TYPE，
# 否则 SDK 的 fdebug.h 会因为 RELEASE / _DEBUG / NDEBUG 一个都没定义而 #error。
# 多配置生成器（Visual Studio / Xcode）走 --config 参数，无需在此设置。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(SMTG_USE_STATIC_CRT ON CACHE BOOL "Use the static MSVC runtime for SDK targets" FORCE)
    # 源码里含中文（界面文案、注释），必须让 MSVC 按 UTF-8 解析，
    # 否则会报 C4819 或把字符串编译成乱码。
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE)
endif()

#------------------------------------------------------------------------------
# VST3 SDK
#------------------------------------------------------------------------------
set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk"
    CACHE PATH "VST3 SDK 根目录（含 pluginterfaces/base/public.sdk）")

if(NOT EXISTS "${VST3_SDK_DIR}/pluginterfaces")
    message(FATAL_ERROR
        "找不到 VST3 SDK：${VST3_SDK_DIR}\n"
        "请执行：git clone --recurse-submodules "
        "https://github.com/steinbergmedia/vst3sdk.git vst3sdk")
endif()

# 只构建我们需要的部分，跳过 VSTGUI / 示例 / 校验器，加快 CI。
#
# ⚠ 选项名必须与 SDK 顶层 CMakeLists.txt 中 option() 声明的完全一致。
#   CMake 对「设置了不存在的变量」不报错，只会静默失效 —— 我们曾把
#   VSTGUI 开关写成 SMTG_ADD_VSTGUI，结果 vstgui4 照旧被 add_subdirectory，
#   它在 Linux 上会强制 find_package(Wayland/X11/Freetype) + pkg_check_modules(
#   xcb xcb-util xcb-cursor xcb-keysyms xcb-xkb xkbcommon xkbcommon-x11
#   glib-2.0 cairo pangocairo pangoft2 fontconfig)，CI 直接卡在配置阶段。
#   下面的名字取自 SDK/CMakeLists.txt:10-13 与 cmake/modules/*.cmake。
set(SMTG_ENABLE_VSTGUI_SUPPORT        OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT       OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK           OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR            OFF CACHE BOOL "" FORCE)
# moduleinfotool 在 public.sdk/samples/vst-utilities 里，我们要它生成 moduleinfo.json
set(SMTG_ADD_VST3_UTILITIES           ON  CACHE BOOL "" FORCE)
set(SMTG_CREATE_MODULE_INFO           ON  CACHE BOOL "" FORCE)

add_subdirectory("${VST3_SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk")

#------------------------------------------------------------------------------
# 源文件（按平台拆分）
#------------------------------------------------------------------------------
set(AP_SOURCES
    source/player.cpp        # 播放核心（纯 C++，跨平台）
    source/pitchshift.cpp    # 固定时长离线变调
    source/tempo.cpp
    source/third_party/minibpm/MiniBpm.cpp
    source/scorebridge.cpp
    source/aplaysdk.cpp      # VST3 处理器 / 控制器 / 工厂（纯 C++）
    # Ogg Vorbis 解码 —— 三端系统框架都没有 Vorbis，所以三端都得自带这一份。
    # 见 source/ogg_vorbis.h。（stb_vorbis 的实现单元由它 #include 进来）
    source/ogg_vorbis.cpp
)

if(APPLE)
    list(APPEND AP_SOURCES
        source/audiofile.mm      # 解码（AVFoundation）
        source/crashguard.mm     # 崩溃取证（signal + backtrace）
        source/gui.mm)           # 界面（AppKit）
elseif(WIN32)
    list(APPEND AP_SOURCES
        source/win/updater.cpp
        source/win/scoreinfo_win.cpp
        "${VST3_SDK_DIR}/vstgui4/vstgui/thirdparty/miniz/miniz.c"
        source/win/audiofile_win.cpp   # 解码（Media Foundation）
        source/win/crashguard_win.cpp  # 崩溃取证（SEH + DbgHelp）
        source/win/video_view.cpp      # 本地/网络视频对照窗口（WebView2）
        source/win/gui_win.cpp)        # 界面（Win32）
elseif(UNIX)
    # Linux：界面走 X11 + Xft；解码用 vendored dr_libs（插件自带编解码器，用户零依赖）
    list(APPEND AP_SOURCES
        source/linux/audiofile_linux.cpp    # 解码（dr_wav / dr_mp3 / dr_flac）
        source/linux/crashguard_linux.cpp   # 崩溃取证（signal + backtrace）
        source/linux/gui_linux.cpp          # 界面（X11 + Xft）
        source/linux/dr_libs_impl.cpp)      # 三个单文件解码库的实现单元

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11 REQUIRED x11)
    pkg_check_modules(XFT REQUIRED xft)
    find_package(Threads REQUIRED)
else()
    message(FATAL_ERROR "当前仅支持 macOS / Windows / Linux")
endif()

#------------------------------------------------------------------------------
# 目标
#------------------------------------------------------------------------------
if(COMMAND smtg_add_vst3plugin)
    if(WIN32)
        smtg_add_vst3plugin(MuseTone PACKAGE_NAME MuseSync SOURCES_LIST ${AP_SOURCES})
    else()
        smtg_add_vst3plugin(MuseTone ${AP_SOURCES})
    endif()
else()
    # 兜底：SDK 未提供辅助宏时，手动建一个按 .vst3 命名的模块库
    add_library(MuseTone MODULE ${AP_SOURCES})
    set_target_properties(MuseTone PROPERTIES PREFIX "" SUFFIX ".vst3")
    if(WIN32)
        set_target_properties(MuseTone PROPERTIES OUTPUT_NAME MuseSync)
    endif()
    target_link_libraries(MuseTone PRIVATE sdk pluginterfaces base)
endif()

target_include_directories(MuseTone PRIVATE
    "${VST3_SDK_DIR}/vstgui4/vstgui/thirdparty/miniz"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    # 源码里用 "public.sdk/..." / "pluginterfaces/..." 全路径形式包含 SDK 头，
    # 所以要把 SDK 根目录加进 include 路径（与 build.sh 的 -I$SDK 对应）
    "${VST3_SDK_DIR}")

if(WIN32 AND WEBVIEW2_SDK_DIR)
    set(_WEBVIEW2_INCLUDE "${WEBVIEW2_SDK_DIR}/build/native/include")
    set(_WEBVIEW2_LIB "")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_WEBVIEW2_LIB "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_WEBVIEW2_LIB "${WEBVIEW2_SDK_DIR}/build/native/x86/WebView2LoaderStatic.lib")
    endif()

    if(EXISTS "${_WEBVIEW2_INCLUDE}/WebView2.h" AND EXISTS "${_WEBVIEW2_LIB}")
        target_include_directories(MuseTone PRIVATE "${_WEBVIEW2_INCLUDE}")
        target_compile_definitions(MuseTone PRIVATE MUSESYNC_HAS_WEBVIEW2=1)
        target_link_libraries(MuseTone PRIVATE "${_WEBVIEW2_LIB}")
        message(STATUS "启用 WebView2 视频播放器：${_WEBVIEW2_LIB}")
    else()
        message(WARNING
            "WEBVIEW2_SDK_DIR 无效，视频播放器将使用降级实现：${WEBVIEW2_SDK_DIR}")
    endif()
endif()

target_compile_definitions(MuseTone PRIVATE MUSETONE_VERSION="${PROJECT_VERSION}")

# SDK 实现库：smtg_add_vst3plugin 只建 bundle，不替你链接这些，
# 必须显式链接（SDK 自带示例插件也是这么写的）。顺序：被依赖者靠后。
target_link_libraries(MuseTone PRIVATE
    sdk sdk_hosting sdk_common pluginterfaces base)

if(WIN32)
    # Media Foundation（解码）+ DbgHelp（崩溃栈）+ 常用 Win32 库
    target_link_libraries(MuseTone PRIVATE
        mfplat mfreadwrite mfuuid
        dbghelp
        ole32 oleaut32 uuid
        user32 gdi32 comctl32 comdlg32 shell32)
    target_link_libraries(MuseTone PRIVATE xmllite shlwapi winhttp bcrypt crypt32)
elseif(APPLE)
    target_link_libraries(MuseTone PRIVATE
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Cocoa")
elseif(UNIX)
    # X11（窗口/绘制）+ Xft（中文与抗锯齿字体）+ 线程
    target_include_directories(MuseTone PRIVATE
        ${X11_INCLUDE_DIRS} ${XFT_INCLUDE_DIRS})
    target_link_directories(MuseTone PRIVATE
        ${X11_LIBRARY_DIRS} ${XFT_LIBRARY_DIRS})
    target_link_libraries(MuseTone PRIVATE
        ${X11_LIBRARIES} ${XFT_LIBRARIES} Threads::Threads)

    # GCC 9 / 10 的 libstdc++ 还没把 std::filesystem 的实现并进主库
    # （GCC 11 才并进去），需要单独链 libstdc++fs。
    # SDK 的 public.sdk/source/vst/hosting/module_linux.cpp 用了 <filesystem>，
    # 它经 sdk_hosting 传进来；Ubuntu 20.04 自带的 GCC 9 少了这一步会报
    # undefined reference to `std::filesystem::...`。
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        find_library(STDCXXFS_LIB stdc++fs)
        if(STDCXXFS_LIB)
            target_link_libraries(MuseTone PRIVATE ${STDCXXFS_LIB})
            message(STATUS "GCC ${CMAKE_CXX_COMPILER_VERSION}：补链 libstdc++fs（std::filesystem）")
        endif()
    endif()

    # 第三方单文件解码库：屏蔽它自身的编译警告，别淹没我们自己的日志
    set_source_files_properties(source/linux/dr_libs_impl.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ogg_vorbis.cpp 里 #include 了 stb_vorbis.c（约 5600 行第三方 C 代码）。
# 它自己会带出一条无害的 -Wtautological-compare，不屏蔽的话每次构建都刷屏，
# 把真正的告警淹掉。这个屏蔽只作用于该文件，不影响我们自己的代码。
if(MSVC)
    set_source_files_properties(source/ogg_vorbis.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(source/ogg_vorbis.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

#------------------------------------------------------------------------------
# Focused regression tests
#------------------------------------------------------------------------------
if(MUSETONE_BUILD_TESTS)
    enable_testing()
    add_executable(musesync_tempo_test tests/tempo_test.cpp source/tempo.cpp
        source/third_party/minibpm/MiniBpm.cpp)
    target_include_directories(musesync_tempo_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source")
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(musesync_tempo_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME musesync_tempo_test COMMAND musesync_tempo_test)
    add_executable(musetone_pitchshift_test
        tests/pitchshift_test.cpp
        source/pitchshift.cpp)
    target_include_directories(musetone_pitchshift_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source")
    add_test(NAME musetone_pitchshift_test COMMAND musetone_pitchshift_test)
    add_executable(musetone_ui_layout_test tests/ui_layout_test.cpp)
    target_include_directories(musetone_ui_layout_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source")
    add_test(NAME musetone_ui_layout_test COMMAND musetone_ui_layout_test)
    add_executable(musetone_scorebridge_test tests/scorebridge_test.cpp source/scorebridge.cpp)
    target_include_directories(musetone_scorebridge_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source" "${VST3_SDK_DIR}")
    add_test(NAME musetone_scorebridge_test COMMAND musetone_scorebridge_test)
    find_program(MUSETONE_NODE_EXECUTABLE node)
    if(MUSETONE_NODE_EXECUTABLE)
        add_test(NAME musetone_score_snapshot_test
            COMMAND "${MUSETONE_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/score_snapshot_test.cjs")
        add_test(NAME musesync_video_script_test
            COMMAND "${MUSETONE_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/video_script_test.cjs")
    endif()
    if(WIN32)
        add_executable(musetone_sync_test tests/sync_test.cpp ${AP_SOURCES}
            "${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp")
        target_compile_definitions(musetone_sync_test PRIVATE MUSETONE_VERSION="${PROJECT_VERSION}")
        target_include_directories(musetone_sync_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/source" "${VST3_SDK_DIR}"
            "${VST3_SDK_DIR}/vstgui4/vstgui/thirdparty/miniz")
        target_link_libraries(musetone_sync_test PRIVATE
            sdk sdk_hosting sdk_common pluginterfaces base
            mfplat mfreadwrite mfuuid dbghelp ole32 oleaut32 uuid
            user32 gdi32 comctl32 comdlg32 shell32 xmllite shlwapi winhttp bcrypt crypt32)
        if(WEBVIEW2_SDK_DIR AND EXISTS "${_WEBVIEW2_INCLUDE}/WebView2.h" AND EXISTS "${_WEBVIEW2_LIB}")
            target_include_directories(musetone_sync_test PRIVATE "${_WEBVIEW2_INCLUDE}")
            target_compile_definitions(musetone_sync_test PRIVATE MUSESYNC_HAS_WEBVIEW2=1)
            target_link_libraries(musetone_sync_test PRIVATE "${_WEBVIEW2_LIB}")
        endif()
        add_test(NAME musetone_sync_test COMMAND musetone_sync_test)
        add_executable(musesync_updater_test tests/updater_test.cpp source/win/updater.cpp)
        target_include_directories(musesync_updater_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source")
        target_link_libraries(musesync_updater_test PRIVATE winhttp bcrypt shell32 ole32 uuid)
        add_test(NAME musesync_updater_test COMMAND musesync_updater_test)
        set_tests_properties(musetone_sync_test PROPERTIES ENVIRONMENT "MUSESYNC_DISABLE_UPDATE_CHECK=1")
    endif()
endif()
