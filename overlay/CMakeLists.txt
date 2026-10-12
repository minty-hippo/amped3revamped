# amped3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(amped3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Every .cpp in src/ and src/native/ (new files are picked up on the next
# configure).
file(GLOB AMPED3_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")

if(WIN32)
    add_executable(amped3 WIN32 ${AMPED3_SOURCES})
else()
    add_executable(amped3 ${AMPED3_SOURCES})
endif()

# miniz (third_party/miniz, MIT): deflate for the patched menu packages
# (src/gui_patch_core.cpp). Plain C.
target_sources(amped3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c PROPERTIES
    COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_TIME;MINIZ_NO_ARCHIVE_APIS;MINIZ_NO_ZLIB_COMPATIBLE_NAMES")


if(WIN32)
    target_link_libraries(amped3 PRIVATE dbghelp pdh)  # crash reports; performance counters (F3)
endif()

# GPU emulation (D3D12 / Vulkan) ships as the rexgpu-xenos plugin DLL.
rexglue_setup_target(amped3 GPU_PLUGINS xenos)

# Native renderer (src/native): our own Vulkan loader and headers, and
# glslang for compiling the translated shaders in the game. setup.ps1 points
# AMPED3_GLSLANG_DIR at the SDK checkout's copy (the same version the SDK
# uses); without it the native renderer preview is compiled out.
set(AMPED3_NATIVE_VK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/vk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/renderer.cpp)
set_source_files_properties(${AMPED3_NATIVE_VK_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan)
# Texture replacements read PNGs with stb_image (third_party/stb, public domain).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/native/texture_replace.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
# Upscalers (XeSS, FSR): the vendors' headers (third_party, unmodified); their
# libraries are loaded at run time (setup.ps1 puts them next to the exe).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/native/upscaler.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan;${CMAKE_CURRENT_SOURCE_DIR}/third_party/xess/inc;${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx_api/include")
# The SDK's Vulkan presenter header needs renderdoc_app.h, which the SDK
# install doesn't ship; take it from the SDK checkout (for live_present.cpp).
set(AMPED3_SDK_SOURCE_DIR "" CACHE PATH "ReXGlue SDK checkout (setup.ps1 sets it)")
if(AMPED3_SDK_SOURCE_DIR AND EXISTS "${AMPED3_SDK_SOURCE_DIR}/thirdparty/renderdoc/renderdoc_app.h")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/live_present.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${AMPED3_SDK_SOURCE_DIR}/thirdparty/renderdoc"
        COMPILE_DEFINITIONS AMPED3_VULKAN_PRESENT=1)
else()
    message(STATUS "amped3: renderdoc_app.h not found; native renderer preview works with Direct3D 12 only")
endif()
set(AMPED3_GLSLANG_DIR "" CACHE PATH "glslang source folder (the SDK's thirdparty/glslang)")
set(AMPED3_GLSLANG_LIBS "")
if(AMPED3_GLSLANG_DIR AND EXISTS "${AMPED3_GLSLANG_DIR}/CMakeLists.txt")
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(_amped3_build_testing "${BUILD_TESTING}")
    set(_amped3_shared "${BUILD_SHARED_LIBS}")
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)  # static, linked into the exe
    add_subdirectory("${AMPED3_GLSLANG_DIR}" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${_amped3_shared}")
    set(BUILD_TESTING "${_amped3_build_testing}" CACHE BOOL "" FORCE)
    foreach(_lib glslang SPIRV MachineIndependent GenericCodeGen OGLCompiler OSDependent)
        if(TARGET ${_lib})
            list(APPEND AMPED3_GLSLANG_LIBS ${_lib})
        endif()
    endforeach()
    target_link_libraries(amped3 PRIVATE ${AMPED3_GLSLANG_LIBS})
    target_compile_definitions(amped3 PRIVATE AMPED3_HAVE_GLSLANG=1)
    message(STATUS "amped3: native renderer shader compiler from ${AMPED3_GLSLANG_DIR}")
else()
    message(STATUS "amped3: AMPED3_GLSLANG_DIR not set; native renderer preview unavailable")
endif()

# Offline shader translator for the native renderer (docs/native-shaders.md).
# Plain C++, no SDK dependency; built alongside the game.
add_executable(amped3-shader-translate
    tools/shader-translate/main.cpp
    src/native/xenos_shader.cpp)

# Texture upscaler (installer\Upscale-Textures.ps1): reads the game's textures,
# runs Real-ESRGAN over them and writes textures\upscaled\*.png for the native
# renderer. Plain C++, no SDK dependency.
add_executable(amped3-texture-upscale
    tools/texture-upscale/main.cpp
    src/gui_patch_core.cpp
    src/native/xenos_texture.cpp
    src/native/texture_replace.cpp
    third_party/miniz/miniz.c)
target_include_directories(amped3-texture-upscale PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

# Frame capture replayer (docs/native-replay.md): draws an F10 capture with
# the native renderer's code paths through Vulkan and writes a PNG.
add_executable(amped3-replay
    tools/replay/main.cpp
    tools/replay/capture.cpp
    src/native/vk.cpp
    src/native/renderer.cpp
    src/native/glsl_compiler.cpp
    src/native/frame_writer.cpp
    src/native/xenos_shader.cpp
    src/native/xenos_texture.cpp
    src/native/texture_replace.cpp
    src/native/upscaler.cpp
    third_party/miniz/miniz.c)
target_include_directories(amped3-replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
if(AMPED3_GLSLANG_LIBS)
    # --glslang internal: compile with the game's compiler.
    target_link_libraries(amped3-replay PRIVATE ${AMPED3_GLSLANG_LIBS})
    target_compile_definitions(amped3-replay PRIVATE AMPED3_HAVE_GLSLANG=1)
endif()
if(NOT WIN32)
    target_link_libraries(amped3-replay PRIVATE dl)
endif()

# The texture pack next to the exe: assets/textures (its README) and, when
# present, assets/local/textures (your own replacements, such as the new
# logo; not part of the published sources). Files of the same name are
# refreshed; others added there are left alone.
add_custom_command(TARGET amped3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures $<TARGET_FILE_DIR:amped3>/textures
    VERBATIM
)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/local/textures)
    add_custom_command(TARGET amped3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/local/textures $<TARGET_FILE_DIR:amped3>/textures
        VERBATIM
    )
endif()

# Starting settings next to the exe. Copied only when missing so the options
# menu's saved choices survive rebuilds.
add_custom_command(TARGET amped3 POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/amped3.defaults.toml
        -DDST=$<TARGET_FILE_DIR:amped3>/amped3.toml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake
    VERBATIM
)
