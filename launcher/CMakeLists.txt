# amped3-launcher: the program you download to install, update and start
# Amped 3 Revamped. Standalone: SDL3 and Dear ImGui (fetched at configure
# time), nothing from the game or the ReXGlue SDK.
#
#   cmake -S launcher -B out/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build out/launcher
#
# Offline: -DFETCHCONTENT_SOURCE_DIR_SDL3=<SDL checkout>
#          -DFETCHCONTENT_SOURCE_DIR_IMGUI=<imgui checkout>

cmake_minimum_required(VERSION 3.25)
project(amped3_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Windows: the static C runtime for everything (SDL included), so the exe
# needs no Visual C++ runtime installed and links against one CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# SDL3, static, only what the launcher uses (window, renderer, controllers,
# file dialogs).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.24
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

# The title animation, built into the program: assets/title.gif as a C array
# (written at configure time, again whenever the GIF changes).
set(TITLE_GIF ${CMAKE_CURRENT_SOURCE_DIR}/assets/title.gif)
set(TITLE_GIF_C ${CMAKE_CURRENT_BINARY_DIR}/title_gif.c)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TITLE_GIF})
if(NOT EXISTS ${TITLE_GIF_C} OR ${TITLE_GIF} IS_NEWER_THAN ${TITLE_GIF_C})
    file(READ ${TITLE_GIF} title_hex HEX)
    string(LENGTH "${title_hex}" title_hex_len)
    math(EXPR title_size "${title_hex_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," title_bytes "${title_hex}")
    # One line per 32 bytes (some compilers dislike very long lines).
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" title_bytes "${title_bytes}")
    file(WRITE ${TITLE_GIF_C}
        "/* Generated from assets/title.gif by CMakeLists.txt */\n"
        "const unsigned char kTitleGif[] = {\n${title_bytes}\n};\n"
        "const unsigned int kTitleGifSize = ${title_size};\n")
endif()

add_executable(amped3-launcher WIN32
    src/main.cpp
    src/app.cpp
    src/title_logo.cpp
    ${TITLE_GIF_C}
    src/platform.cpp
    src/process.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(amped3-launcher PRIVATE
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)
target_link_libraries(amped3-launcher PRIVATE SDL3::SDL3-static)

if(MSVC)
    target_compile_options(amped3-launcher PRIVATE /utf-8)
elseif(UNIX AND NOT APPLE)
    # One file that runs on any distribution of the same architecture.
    target_link_options(amped3-launcher PRIVATE -static-libstdc++ -static-libgcc)
endif()
