// GLSL -> SPIR-V in the game, through the glslang library (the copy in the
// SDK's thirdparty folder, built into amped3.exe when setup.ps1 passes
// AMPED3_GLSLANG_DIR). The offline replayer uses glslangValidator instead.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amped3::native {

// Whether this build has the compiler.
bool GlslCompilerAvailable();

// Vulkan 1.2 / SPIR-V 1.5 target. Thread-safe.
bool CompileGlsl(bool fragment, const std::string& glsl, std::vector<uint32_t>* spirv,
                 std::string* error);

}  // namespace amped3::native
