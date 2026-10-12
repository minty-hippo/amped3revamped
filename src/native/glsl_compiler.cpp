#include "glsl_compiler.h"

#if defined(AMPED3_HAVE_GLSLANG)

#include <mutex>

#include <glslang/Public/ShaderLang.h>
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif

namespace amped3::native {
namespace {

// glslang's default limits (StandAlone/ResourceLimits.cpp), set by name so
// the code doesn't depend on the struct's field order between versions.
TBuiltInResource DefaultResources() {
  TBuiltInResource r{};
  r.maxLights = 32;
  r.maxClipPlanes = 6;
  r.maxTextureUnits = 32;
  r.maxTextureCoords = 32;
  r.maxVertexAttribs = 64;
  r.maxVertexUniformComponents = 4096;
  r.maxVaryingFloats = 64;
  r.maxVertexTextureImageUnits = 32;
  r.maxCombinedTextureImageUnits = 80;
  r.maxTextureImageUnits = 32;
  r.maxFragmentUniformComponents = 4096;
  r.maxDrawBuffers = 32;
  r.maxVertexUniformVectors = 128;
  r.maxVaryingVectors = 8;
  r.maxFragmentUniformVectors = 16;
  r.maxVertexOutputVectors = 16;
  r.maxFragmentInputVectors = 15;
  r.minProgramTexelOffset = -8;
  r.maxProgramTexelOffset = 7;
  r.maxClipDistances = 8;
  r.maxComputeWorkGroupCountX = 65535;
  r.maxComputeWorkGroupCountY = 65535;
  r.maxComputeWorkGroupCountZ = 65535;
  r.maxComputeWorkGroupSizeX = 1024;
  r.maxComputeWorkGroupSizeY = 1024;
  r.maxComputeWorkGroupSizeZ = 64;
  r.maxComputeUniformComponents = 1024;
  r.maxComputeTextureImageUnits = 16;
  r.maxComputeImageUniforms = 8;
  r.maxComputeAtomicCounters = 8;
  r.maxComputeAtomicCounterBuffers = 1;
  r.maxVaryingComponents = 60;
  r.maxVertexOutputComponents = 64;
  r.maxGeometryInputComponents = 64;
  r.maxGeometryOutputComponents = 128;
  r.maxFragmentInputComponents = 128;
  r.maxImageUnits = 8;
  r.maxCombinedImageUnitsAndFragmentOutputs = 8;
  r.maxCombinedShaderOutputResources = 8;
  r.maxFragmentImageUniforms = 8;
  r.maxCombinedImageUniforms = 8;
  r.maxGeometryTextureImageUnits = 16;
  r.maxGeometryOutputVertices = 256;
  r.maxGeometryTotalOutputComponents = 1024;
  r.maxGeometryUniformComponents = 1024;
  r.maxGeometryVaryingComponents = 64;
  r.maxTessControlInputComponents = 128;
  r.maxTessControlOutputComponents = 128;
  r.maxTessControlTextureImageUnits = 16;
  r.maxTessControlUniformComponents = 1024;
  r.maxTessControlTotalOutputComponents = 4096;
  r.maxTessEvaluationInputComponents = 128;
  r.maxTessEvaluationOutputComponents = 128;
  r.maxTessEvaluationTextureImageUnits = 16;
  r.maxTessEvaluationUniformComponents = 1024;
  r.maxTessPatchComponents = 120;
  r.maxPatchVertices = 32;
  r.maxTessGenLevel = 64;
  r.maxViewports = 16;
  r.maxFragmentAtomicCounters = 8;
  r.maxCombinedAtomicCounters = 8;
  r.maxAtomicCounterBindings = 1;
  r.maxFragmentAtomicCounterBuffers = 1;
  r.maxCombinedAtomicCounterBuffers = 1;
  r.maxAtomicCounterBufferSize = 16384;
  r.maxTransformFeedbackBuffers = 4;
  r.maxTransformFeedbackInterleavedComponents = 64;
  r.maxCullDistances = 8;
  r.maxCombinedClipAndCullDistances = 8;
  r.maxSamples = 4;
  r.limits.nonInductiveForLoops = true;
  r.limits.whileLoops = true;
  r.limits.doWhileLoops = true;
  r.limits.generalUniformIndexing = true;
  r.limits.generalAttributeMatrixVectorIndexing = true;
  r.limits.generalVaryingIndexing = true;
  r.limits.generalSamplerIndexing = true;
  r.limits.generalVariableIndexing = true;
  r.limits.generalConstantMatrixVectorIndexing = true;
  return r;
}

std::once_flag g_init;
std::mutex g_mutex;  // glslang's global state isn't safe for concurrent compiles

}  // namespace

bool GlslCompilerAvailable() { return true; }

bool CompileGlsl(bool fragment, const std::string& glsl, std::vector<uint32_t>* spirv,
                 std::string* error) {
  std::call_once(g_init, [] { glslang::InitializeProcess(); });
  std::lock_guard lock(g_mutex);
  static const TBuiltInResource resources = DefaultResources();
  const EShLanguage stage = fragment ? EShLangFragment : EShLangVertex;
  glslang::TShader shader(stage);
  const char* source = glsl.c_str();
  shader.setStrings(&source, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&resources, 450, false, messages)) {
    *error = shader.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    *error = program.getInfoLog();
    return false;
  }
  spirv->clear();
  glslang::SpvOptions options;
  options.disableOptimizer = true;
  glslang::GlslangToSpv(*program.getIntermediate(stage), *spirv, &options);
  if (spirv->empty()) {
    *error = "no SPIR-V generated";
    return false;
  }
  return true;
}

}  // namespace amped3::native

#else  // !AMPED3_HAVE_GLSLANG

namespace amped3::native {
bool GlslCompilerAvailable() { return false; }
bool CompileGlsl(bool, const std::string&, std::vector<uint32_t>*, std::string* error) {
  *error = "this build has no GLSL compiler (setup.ps1 builds it in with the SDK's glslang)";
  return false;
}
}  // namespace amped3::native

#endif
