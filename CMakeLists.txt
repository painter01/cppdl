cmake_minimum_required(VERSION 3.16)
project(cppdl LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMMON_SOURCES
    src/url.cpp
    src/http.cpp
    src/downloader.cpp
    src/format.cpp
)

# 控制台版
add_executable(cppdl src/main.cpp ${COMMON_SOURCES})
# Win32 窗口版（附加 app.rc 把 ico 图标嵌入 exe）
# app.manifest：comctl32 v6 视觉样式 + DPI 感知，CMake 自动转为 /MANIFESTINPUT 嵌入
add_executable(cppdl_gui WIN32 src/gui_main.cpp ${COMMON_SOURCES} src/app.rc src/app.manifest)

foreach(target cppdl cppdl_gui)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_include_directories(${target} PRIVATE res)
    target_link_libraries(${target} PRIVATE winhttp shell32 comctl32 comdlg32)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # 静态链接运行时，生成的 exe 不依赖 MinGW DLL
            target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endif()
endforeach()

if(NOT MSVC)
    # MinGW 的 -mwindows 默认入口是 WinMain，需切换到 wWinMain
    target_link_options(cppdl_gui PRIVATE -municode)
endif()
