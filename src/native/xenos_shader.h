// Xenos shader microcode -> Vulkan GLSL translator for the native renderer.
//
// Step 3 of docs/native-renderer-plan.md. Self-contained (no SDK headers) so
// the same code runs offline in tools/shader-translate and, later, in the game
// as the run-time fallback for shaders that were not translated ahead of time.
// The bit layouts follow rex/graphics/format/ucode.h; the behaviour of each
// instruction follows the comments there and the SDK's SPIR-V translator.
//
// The interface between the translated shaders and the renderer (descriptor
// sets, constant buffers, system constants) is documented in
// docs/native-shaders.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amped3::native {

enum class ShaderStage { kVertex, kPixel };

// One texture binding. Each distinct (fetch constant, dimension, filter
// override) combination used by a shader gets its own combined image sampler.
struct TextureBinding {
  uint32_t binding = 0;         // binding number within the stage's texture set
  uint32_t fetch_constant = 0;  // tf0..tf31
  uint32_t dimension = 1;       // 0 = 1D, 1 = 2D, 2 = 3D / stacked, 3 = cube
  // Filter overrides from the instruction; -1 = use the fetch constant.
  int mag_filter = -1, min_filter = -1, mip_filter = -1, aniso_filter = -1;
};

struct ShaderInfo {
  ShaderStage stage = ShaderStage::kVertex;
  uint64_t ucode_hash = 0;  // FNV-1a of the microcode (host-order dwords)
  uint32_t ucode_dwords = 0;
  uint32_t register_count = 0;       // temporaries r0..rN-1 the shader touches
  bool dynamic_registers = false;    // aL-relative register addressing
  bool dynamic_constants = false;    // a0 / aL-relative float constant addressing
  uint32_t relative_constant_base = 256;  // lowest a0-relative offset (a bone palette's start)
  uint32_t float_constants[8] = {};  // bitmap of c0..c255 read (static only)
  bool uses_bool_constants = false;
  bool uses_loop_constants = false;
  uint32_t vertex_fetch_constants[3] = {};  // bitmap of vf0..vf95
  std::vector<uint32_t> vertex_formats;     // xenos::VertexFormat values used
  std::vector<TextureBinding> textures;
  uint32_t interpolators_written = 0;  // VS: o0..o15
  bool writes_point_size = false;      // VS: oPts.x
  uint32_t colors_written = 0;         // PS: oC0..oC3
  bool writes_depth = false;           // PS: oDepth
  bool kills = false;                  // PS: kill instructions
  bool has_jumps = false;              // jumps, calls or loops (dispatch loop used)
  uint32_t cf_count = 0, alu_count = 0, vfetch_count = 0, tfetch_count = 0;
  // Things translated approximately (behaviour differs from the Xenos).
  std::vector<std::string> approximations;
};

struct TranslateResult {
  bool ok = false;
  std::string error;   // set when !ok
  std::string glsl;    // GLSL 450 for glslang (Vulkan semantics)
  std::string disasm;  // microcode listing, also embedded in the GLSL as comments
  ShaderInfo info;
};

struct TranslateOptions {
  // Shader blobs as passed to CreateVertexShader hold placeholder vertex
  // fetches (format 0, stride 0, vf95); Direct3D patches them in place for the
  // vertex declaration when the shader is first drawn with it. The renderer
  // must translate the patched microcode. For offline checks of blob
  // templates, this treats a placeholder as a 4 x float32 fetch.
  bool placeholder_vertex_fetch = false;
  // Diagnostics: vertex shaders also store, when XE_FLAG_DEBUG_STORE is set,
  // each vertex's position before and after the viewport transform and its
  // index to a buffer at set 1, binding 1 (4 x vec4 per vertex, the first
  // 4096 vertices). Needs vertexPipelineStoresAndAtomics.
  bool debug_vertex_stores = false;
  // Byte swap implementation for guest memory reads: 1 (default) selects,
  // 0 branches, 2 bitfieldExtract bytes, 3 vector-shifted bytes. The Intel
  // Arc driver (101.8992) miscompiles forms 0 and 3 in the board trail's
  // vertex shader: the low half of an 8-in-32 swap keeps the unswapped bytes
  // (docs/native-renderer-plan.md).
  int swap_variant = 1;
  // Motion vectors (vertex shaders): the program runs twice, with this
  // frame's float constants and with last frame's (set 0, binding 2, the
  // pixel stage's slot, unused by the velocity pixel shader), and the shader
  // outputs both clip positions (location 0 = this frame, 1 = last frame)
  // instead of the interpolators. gl_Position is the same as the normal
  // variant's, so a depth-EQUAL pass matches the scene.
  bool velocity = false;
};

// `ucode` is the microcode in host byte order.
TranslateResult TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dword_count,
                                const TranslateOptions& options = {});

// Pulls the microcode out of a compiled shader blob as passed to
// D3DDevice_CreateVertexShader / CreatePixelShader (big-endian, as in guest
// memory): +0 magic (0x102A0E01 VS, 0x102A0E00 PS), +4 microcode offset,
// +8 microcode size in bytes.
bool ExtractBlobMicrocode(const uint8_t* blob, size_t size, ShaderStage* stage,
                          std::vector<uint32_t>* ucode, std::string* error);

uint64_t HashMicrocode(const uint32_t* ucode, size_t dword_count);

}  // namespace amped3::native
