# AMD FidelityFX FSR 3.1 upscaler for Linux: libamd_fidelityfx_vk.so, the
# FidelityFX API runtime the game loads (src/native/upscaler.cpp), built from
# the sources in AMD's FidelityFX SDK v1.1.4 release (MIT license). AMD only
# ships that runtime as a Windows DLL; this builds the same API (ffxCreateContext,
# ffxQuery, ffxDispatch, ffxDestroyContext) with only the upscaler in it.
#
#   cmake -S linux/fidelityfx -B <build> -DFFX_SDK_DIR=<FidelityFX-SDK v1.1.4> \
#         -DGLSLANG_SOURCE_DIR=<glslang sources> -DVULKAN_INCLUDE_DIR=<headers>
#
# (setup.sh does this: it takes the SDK's sources from AMD's repository and
# applies fidelityfx-sdk-linux.patch.) Steps: build the SDK's shader compiler
# (FidelityFX-SC, which the patch makes build on Linux) and glslang's
# command line compiler, compile the FSR 3.1 shaders (SPIR-V, each
# permutation reflected into a header), then the library.

cmake_minimum_required(VERSION 3.20)
project(amd_fidelityfx_vk_linux C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FFX_SDK_DIR "" CACHE PATH "The FidelityFX SDK v1.1.4 release, extracted (has sdk/ and ffx-api/)")
set(GLSLANG_SOURCE_DIR "" CACHE PATH "glslang's sources (the ReXGlue SDK's thirdparty/glslang)")
set(GLSLANG_VALIDATOR "" CACHE FILEPATH "Or a glslang command line compiler already built")
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Vulkan headers (vulkan/vulkan.h)")
foreach(v FFX_SDK_DIR VULKAN_INCLUDE_DIR)
  if(NOT ${v})
    message(FATAL_ERROR "${v} not set")
  endif()
endforeach()

# glslang's command line compiler, without SPIRV-Tools (no optimizer).
if(GLSLANG_VALIDATOR)
  set(GLSLANG_EXE ${GLSLANG_VALIDATOR})
  set(GLSLANG_DEP)
elseif(GLSLANG_SOURCE_DIR)
  set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  add_subdirectory(${GLSLANG_SOURCE_DIR} ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
  # Its name: glslang-standalone in newer glslang, glslangValidator in the
  # one the ReXGlue SDK pins.
  if(TARGET glslang-standalone)
    set(GLSLANG_DEP glslang-standalone)
  elseif(TARGET glslangValidator)
    set(GLSLANG_DEP glslangValidator)
  else()
    message(FATAL_ERROR "glslang at ${GLSLANG_SOURCE_DIR} has no command line compiler target")
  endif()
  set(GLSLANG_EXE $<TARGET_FILE:${GLSLANG_DEP}>)
else()
  message(FATAL_ERROR "GLSLANG_SOURCE_DIR (or GLSLANG_VALIDATOR) not set")
endif()

set(SDK ${FFX_SDK_DIR}/sdk)
set(API ${FFX_SDK_DIR}/ffx-api)
set(SC ${SDK}/tools/ffx_shader_compiler)
set(COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/ffx_linux_compat.h)

# The sources name one folder "FidelityFx" (Windows doesn't mind the case).
set(CASE_DIR ${CMAKE_BINARY_DIR}/case_include)
file(MAKE_DIRECTORY ${CASE_DIR})
if(NOT EXISTS ${CASE_DIR}/FidelityFx)
  file(CREATE_LINK ${SDK}/include/FidelityFX ${CASE_DIR}/FidelityFx SYMBOLIC)
endif()

# --- The shader compiler ------------------------------------------------------
add_executable(ffx_sc
  ${SC}/src/ffx_sc.cpp
  ${SC}/src/glsl_compiler.cpp
  ${SC}/src/utils.cpp
  ${SC}/libs/MD5/md5.cpp
  ${SC}/libs/SPIRV-Reflect/spirv_reflect.c
  ${SC}/libs/tiny-process-library/process.cpp
  ${SC}/libs/tiny-process-library/process_unix.cpp)
target_include_directories(ffx_sc PRIVATE
  ${SC}/src ${SC}/libs/MD5 ${SC}/libs/SPIRV-Reflect ${SC}/libs/tiny-process-library)
target_compile_options(ffx_sc PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(ffx_sc PRIVATE Threads::Threads)

# --- FSR 3.1 shaders ------------------------------------------------------------
# Arguments of the SDK's CMakeCompileFSR3UpscalerShaders.txt and
# backends/vk/CMakeShadersFSR3Upscaler.txt. No -Os: that needs glslang built
# with SPIRV-Tools' optimizer; the driver optimizes the SPIR-V anyway.
set(GPU ${SDK}/include/FidelityFX/gpu)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
set(SC_ARGS
  -reflection -deps=gcc -DFFX_GPU=1
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
  -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
  -compiler=glslang -e CS --target-env vulkan1.2 -S comp -DFFX_GLSL=1
  "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}"
  "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}"
  "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
  "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}"
  "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}"
  "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
  -glslangexe=${GLSLANG_EXE}
  -I${GPU} -I${GPU}/fsr3upscaler
  -output=${SHADER_OUT})
file(GLOB FSR3_SHADERS ${SDK}/src/backends/vk/shaders/fsr3upscaler/*.glsl)
set(SHADER_HEADERS)
foreach(shader ${FSR3_SHADERS})
  get_filename_component(name ${shader} NAME_WE)
  # Variants (the same four as the SDK): wave32/64, without/with 16-bit math.
  foreach(variant "" "_wave64" "_16bit" "_wave64_16bit")
    if(variant MATCHES "16bit")
      set(half -DFFX_HALF=1)
    else()
      set(half -DFFX_HALF=0)
    endif()
    set(out ${SHADER_OUT}/${name}${variant}_permutations.h)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ffx_sc ${SC_ARGS} -name=${name}${variant} ${half} ${shader}
      DEPENDS ffx_sc ${GLSLANG_DEP} ${shader}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "FSR 3.1 shaders: ${name}${variant}"
      VERBATIM)
    list(APPEND SHADER_HEADERS ${out})
  endforeach()
endforeach()
add_custom_target(fsr3_shaders DEPENDS ${SHADER_HEADERS})

# --- The runtime library ----------------------------------------------------------
file(GLOB FFX_SHARED ${SDK}/src/shared/*.cpp)
add_library(amd_fidelityfx_vk SHARED
  ${API}/src/ffx_api.cpp
  ${API}/src/backends.cpp
  ${API}/src/validation.cpp
  ${API}/src/ffx_provider_fsr3upscale.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ffx_provider_linux.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ffx_vulkan_linux.cpp
  ${FFX_SHARED}
  ${SDK}/src/backends/vk/ffx_vk.cpp
  ${SDK}/src/backends/shared/ffx_shader_blobs.cpp
  ${SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
  ${SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp)
add_dependencies(amd_fidelityfx_vk fsr3_shaders)
target_include_directories(amd_fidelityfx_vk PRIVATE
  ${API}/include ${API}/src
  ${SDK}/include ${CASE_DIR}
  ${SDK}/src ${SDK}/src/shared ${SDK}/src/backends/shared ${SDK}/src/backends/shared/blob_accessors
  ${SDK}/src/components ${SDK}/src/backends/vk
  ${SHADER_OUT}
  ${VULKAN_INCLUDE_DIR})
target_compile_definitions(amd_fidelityfx_vk PRIVATE
  FFX_BACKEND_VK FFX_FSR3UPSCALER FFX_GCC
  FFXDLL_VERSION_MAJOR=1 FFXDLL_VERSION_MINOR=0 FFXDLL_VERSION_PATCH=1
  FFXDLL_VERSION_API=VK CI_PIPELINE_ID=0)
target_compile_options(amd_fidelityfx_vk PRIVATE
  -include ${COMPAT} -fvisibility=hidden -w)
target_link_options(amd_fidelityfx_vk PRIVATE -Wl,--no-undefined -Wl,-z,defs)
target_link_libraries(amd_fidelityfx_vk PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(amd_fidelityfx_vk PROPERTIES PREFIX "lib")
