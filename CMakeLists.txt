cmake_minimum_required(VERSION 3.20)
project(SdrLuminance LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "本项目仅支持 MSVC 工具链")
endif()

# 供 CMP0091 使用：静态链接 CRT，产出无外部运行时依赖的单文件 exe
cmake_policy(SET CMP0091 NEW)

# ---------------------------------------------------------------------------
# 公共编译设置
# ---------------------------------------------------------------------------
function(sdr_apply_common_settings target)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        # NTDDI_WIN11_GA —— 必须定义，否则 wingdi.h 中的
        # DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 被 #if 屏蔽而不可见
        NTDDI_VERSION=0x0A00000F
        _WIN32_WINNT=0x0A00
    )

    target_compile_options(${target} PRIVATE
        /W4 /utf-8 /permissive-
        $<$<CONFIG:Release>:/O2 /GL /Gy /Gw>
    )

    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )

    set_property(TARGET ${target} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
add_executable(SdrLuminance WIN32
    src/main.cpp
    src/autostart.cpp
    src/config.cpp
    src/hotkey.cpp
    src/icon.cpp
    src/sdr_api.cpp
    src/tray.cpp
    src/ui_main.cpp
    src/ui_settings.cpp
)
sdr_apply_common_settings(SdrLuminance)
target_link_libraries(SdrLuminance PRIVATE user32 gdi32 comctl32 shell32 advapi32 shlwapi)

# ---------------------------------------------------------------------------
# 探针（回归验证工具，控制台程序）
# ---------------------------------------------------------------------------
add_executable(probe tools/probe.cpp)
sdr_apply_common_settings(probe)
target_link_libraries(probe PRIVATE user32)
