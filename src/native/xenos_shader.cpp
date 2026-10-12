// Xenos shader microcode -> Vulkan GLSL. See xenos_shader.h and
// docs/native-shaders.md.

#include "xenos_shader.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <regex>
#include <set>

namespace amped3::native {
namespace {

// --- Formatting ----------------------------------------------------------------

std::string Fmt(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n < int(sizeof(buffer))) return std::string(buffer, size_t(n < 0 ? 0 : n));
  std::string big(size_t(n) + 1, '\0');
  va_start(args, format);
  std::vsnprintf(big.data(), big.size(), format, args);
  va_end(args);
  big.resize(size_t(n));
  return big;
}

constexpr uint32_t Bits(uint32_t v, uint32_t shift, uint32_t count) {
  return (v >> shift) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1));
}
constexpr int32_t SignedBits(uint32_t v, uint32_t shift, uint32_t count) {
  uint32_t u = Bits(v, shift, count);
  uint32_t sign = 1u << (count - 1);
  return int32_t(u ^ sign) - int32_t(sign);
}

const char kComp[] = "xyzw";

// --- Microcode layouts (rex/graphics/format/ucode.h) ---------------------------

enum CfOp : uint32_t {
  kCfNop = 0, kCfExec = 1, kCfExecEnd = 2, kCfCondExec = 3, kCfCondExecEnd = 4,
  kCfCondExecPred = 5, kCfCondExecPredEnd = 6, kCfLoopStart = 7, kCfLoopEnd = 8,
  kCfCondCall = 9, kCfReturn = 10, kCfCondJmp = 11, kCfAlloc = 12,
  kCfCondExecPredClean = 13, kCfCondExecPredCleanEnd = 14, kCfMarkVsFetchDone = 15,
};

struct Cf {
  uint32_t w0 = 0, w1 = 0;
  uint32_t op() const { return Bits(w1, 12, 4); }
  bool is_exec() const {
    switch (op()) {
      case kCfExec: case kCfExecEnd: case kCfCondExec: case kCfCondExecEnd: case kCfCondExecPred:
      case kCfCondExecPredEnd: case kCfCondExecPredClean: case kCfCondExecPredCleanEnd:
        return true;
      default:
        return false;
    }
  }
  bool is_end() const {
    uint32_t o = op();
    return o == kCfExecEnd || o == kCfCondExecEnd || o == kCfCondExecPredEnd ||
           o == kCfCondExecPredCleanEnd;
  }
  // Exec family.
  uint32_t exec_address() const { return Bits(w0, 0, 12); }
  uint32_t exec_count() const { return Bits(w0, 12, 3); }
  uint32_t exec_sequence() const { return Bits(w0, 16, 12); }
  uint32_t bool_address() const { return Bits(w1, 2, 8); }  // cexec, ccall, cjmp
  bool condition() const { return Bits(w1, 10, 1) != 0; }   // cexec, pred, ccall, cjmp, endloop
  // Loops.
  uint32_t loop_address() const { return Bits(w0, 0, 13); }
  bool loop_is_repeat() const { return Bits(w0, 13, 1) != 0; }
  uint32_t loop_id() const { return Bits(w0, 16, 5); }
  bool loop_predicated_break() const { return Bits(w0, 21, 1) != 0; }
  // Call / jump.
  uint32_t jump_address() const { return Bits(w0, 0, 13); }
  bool jump_unconditional() const { return Bits(w0, 13, 1) != 0; }
  bool jump_predicated() const { return Bits(w0, 14, 1) != 0; }
  // Alloc.
  uint32_t alloc_size() const { return Bits(w0, 0, 3); }
  uint32_t alloc_type() const { return Bits(w1, 9, 2); }
};

void UnpackCfPair(const uint32_t* d, Cf* out) {
  out[0].w0 = d[0];
  out[0].w1 = d[1] & 0xFFFF;
  out[1].w0 = (d[1] >> 16) | (d[2] << 16);
  out[1].w1 = d[2] >> 16;
}

enum FetchOp : uint32_t {
  kVertexFetch = 0, kTextureFetch = 1, kGetBorderColorFrac = 16, kGetComputedLod = 17,
  kGetGradients = 18, kGetWeights = 19, kSetTextureLod = 24, kSetGradientsH = 25,
  kSetGradientsV = 26,
};

struct Fetch {
  uint32_t d[3];
  uint32_t op() const { return Bits(d[0], 0, 5); }
  uint32_t src() const { return Bits(d[0], 5, 6); }
  bool src_rel() const { return Bits(d[0], 11, 1) != 0; }
  uint32_t dst() const { return Bits(d[0], 12, 6); }
  bool dst_rel() const { return Bits(d[0], 18, 1) != 0; }
  uint32_t dst_swizzle() const { return Bits(d[1], 0, 12); }
  bool predicated() const { return Bits(d[1], 31, 1) != 0; }
  bool pred_condition() const { return Bits(d[2], 31, 1) != 0; }
  // Vertex fetch.
  uint32_t vf_constant() const { return Bits(d[0], 20, 5) * 3 + Bits(d[0], 25, 2); }
  uint32_t vf_src_component() const { return Bits(d[0], 30, 2); }
  bool vf_signed() const { return Bits(d[1], 12, 1) != 0; }
  bool vf_integer() const { return Bits(d[1], 13, 1) != 0; }  // num_format_all = 1
  uint32_t vf_signed_rf_mode() const { return Bits(d[1], 14, 1); }
  bool vf_index_rounded() const { return Bits(d[1], 15, 1) != 0; }
  uint32_t vf_format() const { return Bits(d[1], 16, 6); }
  int vf_exp_adjust() const { return SignedBits(d[1], 24, 6); }
  bool vf_mini() const { return Bits(d[1], 30, 1) != 0; }
  uint32_t vf_stride() const { return Bits(d[2], 0, 8); }
  int32_t vf_offset() const { return SignedBits(d[2], 8, 23); }
  // Texture fetch.
  uint32_t tf_constant() const { return Bits(d[0], 20, 5); }
  bool tf_unnormalized() const { return Bits(d[0], 25, 1) != 0; }
  uint32_t tf_src_swizzle() const { return Bits(d[0], 26, 6); }
  uint32_t tf_mag() const { return Bits(d[1], 12, 2); }
  uint32_t tf_min() const { return Bits(d[1], 14, 2); }
  uint32_t tf_mip() const { return Bits(d[1], 16, 2); }
  uint32_t tf_aniso() const { return Bits(d[1], 18, 3); }
  bool tf_computed_lod() const { return Bits(d[1], 28, 1) != 0; }
  bool tf_register_lod() const { return Bits(d[1], 29, 1) != 0; }
  bool tf_register_gradients() const { return Bits(d[2], 0, 1) != 0; }
  float tf_lod_bias() const { return float(SignedBits(d[2], 2, 7)) / 16.0f; }
  uint32_t tf_dimension() const { return Bits(d[2], 14, 2); }
  float tf_offset(int i) const { return float(SignedBits(d[2], 16 + 5 * i, 5)) * 0.5f; }
};

// Vertex formats (xenos::VertexFormat).
enum VertexFormat : uint32_t {
  kVf_8_8_8_8 = 6, kVf_2_10_10_10 = 7, kVf_10_11_11 = 16, kVf_11_11_10 = 17, kVf_16_16 = 25,
  kVf_16_16_16_16 = 26, kVf_16_16_FLOAT = 31, kVf_16_16_16_16_FLOAT = 32, kVf_32 = 33,
  kVf_32_32 = 34, kVf_32_32_32_32 = 35, kVf_32_FLOAT = 36, kVf_32_32_FLOAT = 37,
  kVf_32_32_32_32_FLOAT = 38, kVf_32_32_32_FLOAT = 57,
};

const char* VertexFormatName(uint32_t f) {
  switch (f) {
    case kVf_8_8_8_8: return "8_8_8_8";
    case kVf_2_10_10_10: return "2_10_10_10";
    case kVf_10_11_11: return "10_11_11";
    case kVf_11_11_10: return "11_11_10";
    case kVf_16_16: return "16_16";
    case kVf_16_16_16_16: return "16_16_16_16";
    case kVf_16_16_FLOAT: return "16_16_FLOAT";
    case kVf_16_16_16_16_FLOAT: return "16_16_16_16_FLOAT";
    case kVf_32: return "32";
    case kVf_32_32: return "32_32";
    case kVf_32_32_32_32: return "32_32_32_32";
    case kVf_32_FLOAT: return "32_FLOAT";
    case kVf_32_32_FLOAT: return "32_32_FLOAT";
    case kVf_32_32_32_32_FLOAT: return "32_32_32_32_FLOAT";
    case kVf_32_32_32_FLOAT: return "32_32_32_FLOAT";
    default: return nullptr;
  }
}

struct Alu {
  uint32_t d[3];
  uint32_t vector_dest() const { return Bits(d[0], 0, 6); }
  bool vector_dest_rel() const { return Bits(d[0], 6, 1) != 0; }
  bool abs_constants() const { return Bits(d[0], 7, 1) != 0; }
  uint32_t scalar_dest() const { return Bits(d[0], 8, 6); }
  bool scalar_dest_rel() const { return Bits(d[0], 14, 1) != 0; }
  bool is_export() const { return Bits(d[0], 15, 1) != 0; }
  uint32_t vector_mask() const { return Bits(d[0], 16, 4); }
  uint32_t scalar_mask() const { return Bits(d[0], 20, 4); }
  bool vector_clamp() const { return Bits(d[0], 24, 1) != 0; }
  bool scalar_clamp() const { return Bits(d[0], 25, 1) != 0; }
  uint32_t scalar_op() const { return Bits(d[0], 26, 6); }
  uint32_t src_swizzle(int i) const { return Bits(d[1], 16 - 8 * (i - 1), 8); }
  bool src_negate(int i) const { return Bits(d[1], 26 - (i - 1), 1) != 0; }
  bool pred_condition() const { return Bits(d[1], 27, 1) != 0; }
  bool predicated() const { return Bits(d[1], 28, 1) != 0; }
  bool const_a0_relative() const { return Bits(d[1], 29, 1) != 0; }
  bool const_1_rel() const { return Bits(d[1], 30, 1) != 0; }
  bool const_0_rel() const { return Bits(d[1], 31, 1) != 0; }
  uint32_t src_reg(int i) const { return Bits(d[2], 16 - 8 * (i - 1), 8); }
  uint32_t vector_op() const { return Bits(d[2], 24, 5); }
  bool src_is_temp(int i) const { return Bits(d[2], 31 - (i - 1), 1) != 0; }
  bool src_const_relative(int i) const {
    switch (i) {
      case 1: return const_0_rel();
      case 2: return src_is_temp(1) ? const_0_rel() : const_1_rel();
      default: return (src_is_temp(1) && src_is_temp(2)) ? const_0_rel() : const_1_rel();
    }
  }
  uint32_t scalar_const_op_temp_reg() const {
    return (scalar_op() & 1) | (uint32_t(src_is_temp(3)) << 1) | (src_swizzle(3) & 0x3C);
  }
  // Export component sources.
  uint32_t vector_result_mask() const {
    return is_export() ? (vector_mask() & ~scalar_mask()) : vector_mask();
  }
  uint32_t scalar_result_mask() const {
    return is_export() ? (scalar_mask() & ~vector_mask()) : scalar_mask();
  }
  uint32_t const0_mask() const {
    return (is_export() && scalar_dest_rel()) ? (0xF & ~(vector_mask() | scalar_mask())) : 0;
  }
  uint32_t const1_mask() const { return is_export() ? (vector_mask() & scalar_mask()) : 0; }
};

// Vector opcodes.
enum VectorOp : uint32_t {
  kAdd = 0, kMul, kMax, kMin, kSeq, kSgt, kSge, kSne, kFrc, kTrunc, kFloor, kMad, kCndEq, kCndGe,
  kCndGt, kDp4, kDp3, kDp2Add, kCube, kMax4, kSetpEqPush, kSetpNePush, kSetpGtPush, kSetpGePush,
  kKillEq, kKillGt, kKillGe, kKillNe, kDst, kMaxA,
};
const char* const kVectorNames[32] = {
    "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor",
    "mad", "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
    "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne",
    "dst", "maxa", "opcode_30", "opcode_31"};
const uint8_t kVectorOperands[32] = {2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 3, 3, 3, 3, 2,
                                     2, 3, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0};

// Scalar opcodes.
enum ScalarOp : uint32_t {
  kAdds = 0, kAddsPrev, kMuls, kMulsPrev, kMulsPrev2, kMaxs, kMins, kSeqs, kSgts, kSges, kSnes,
  kFrcs, kTruncs, kFloors, kExp, kLogc, kLog, kRcpc, kRcpf, kRcp, kRsqc, kRsqf, kRsq, kMaxAs,
  kMaxAsf, kSubs, kSubsPrev, kSetpEq, kSetpNe, kSetpGt, kSetpGe, kSetpInv, kSetpPop, kSetpClr,
  kSetpRstr, kKillsEq, kKillsGt, kKillsGe, kKillsNe, kKillsOne, kSqrt, kOpcode41, kMulsc0,
  kMulsc1, kAddsc0, kAddsc1, kSubsc0, kSubsc1, kSin, kCos, kRetainPrev,
};
struct ScalarInfo {
  const char* name;
  uint8_t operands;  // 0, 1 (from src3), 2 (constant + temporary)
  bool two_component;
};
const ScalarInfo kScalarInfo[64] = {
    {"adds", 1, true}, {"adds_prev", 1, false}, {"muls", 1, true}, {"muls_prev", 1, false},
    {"muls_prev2", 1, true}, {"maxs", 1, true}, {"mins", 1, true}, {"seqs", 1, false},
    {"sgts", 1, false}, {"sges", 1, false}, {"snes", 1, false}, {"frcs", 1, false},
    {"truncs", 1, false}, {"floors", 1, false}, {"exp", 1, false}, {"logc", 1, false},
    {"log", 1, false}, {"rcpc", 1, false}, {"rcpf", 1, false}, {"rcp", 1, false},
    {"rsqc", 1, false}, {"rsqf", 1, false}, {"rsq", 1, false}, {"maxas", 1, true},
    {"maxasf", 1, true}, {"subs", 1, true}, {"subs_prev", 1, false}, {"setp_eq", 1, false},
    {"setp_ne", 1, false}, {"setp_gt", 1, false}, {"setp_ge", 1, false}, {"setp_inv", 1, false},
    {"setp_pop", 1, false}, {"setp_clr", 0, false}, {"setp_rstr", 1, false},
    {"kills_eq", 1, false}, {"kills_gt", 1, false}, {"kills_ge", 1, false},
    {"kills_ne", 1, false}, {"kills_one", 1, false}, {"sqrt", 1, false}, {"opcode_41", 0, false},
    {"mulsc", 2, false}, {"mulsc", 2, false}, {"addsc", 2, false}, {"addsc", 2, false},
    {"subsc", 2, false}, {"subsc", 2, false}, {"sin", 1, false}, {"cos", 1, false},
    {"retain_prev", 0, false}, {"opcode_51", 0, false}, {"opcode_52", 0, false},
    {"opcode_53", 0, false}, {"opcode_54", 0, false}, {"opcode_55", 0, false},
    {"opcode_56", 0, false}, {"opcode_57", 0, false}, {"opcode_58", 0, false},
    {"opcode_59", 0, false}, {"opcode_60", 0, false}, {"opcode_61", 0, false},
    {"opcode_62", 0, false}, {"opcode_63", 0, false},
};

// Export registers.
constexpr uint32_t kExportPosition = 62;
constexpr uint32_t kExportPointSize = 63;
constexpr uint32_t kExportDepth = 61;
constexpr uint32_t kExportAddress = 32;

constexpr const char* kFltMax = "3.402823466e+38";

// --- Translator ---------------------------------------------------------------

class Translator {
 public:
  Translator(ShaderStage stage, const uint32_t* ucode, size_t count,
             const TranslateOptions& options)
      : stage_(stage), ucode_(ucode), count_(count), options_(options) {}

  bool Run(TranslateResult& out);

 private:
  bool vs() const { return stage_ == ShaderStage::kVertex; }

  bool Fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    return false;
  }
  void Line(const std::string& s) {
    body_.append(size_t(indent_) * 2, ' ');
    body_ += s;
    body_ += '\n';
  }
  // Listing line; GLSL storage names are shown as microcode register names.
  void Disasm(const std::string& s) {
    static const std::pair<std::regex, const char*> kNames[] = {
        {std::regex(R"(r\[XE_REL\((\d+)\)\])"), "r[aL+$1]"},
        {std::regex(R"(r\[(\d+)\])"), "r$1"},
        {std::regex(R"(xe_fc\.c\[\(xe_a0 \+ (\d+)\) & 255\])"), "c[a0+$1]"},
        {std::regex(R"(xe_fc\.c\[\(xe_aL\.x \+ (\d+)\) & 255\])"), "c[aL+$1]"},
        {std::regex(R"(xe_fc\.c\[(\d+)\])"), "c$1"},
        {std::regex(R"(xe_oC\[(\d+)\])"), "oC$1"},
        {std::regex(R"(xe_o\[(\d+)\])"), "o$1"},
        {std::regex(R"(xe_pos)"), "oPos"},
        {std::regex(R"(xe_misc)"), "oPts"},
        {std::regex(R"(xe_depth)"), "oDepth"},
    };
    std::string text = s;
    for (const auto& [re, name] : kNames) text = std::regex_replace(text, re, name);
    disasm_ += text;
    disasm_ += '\n';
    Line("// " + text);
  }

  bool Scan();
  bool EmitControlFlow(uint32_t cf_index, const Cf& cf);
  bool EmitExec(const Cf& cf);
  bool EmitAlu(const Alu& alu);
  bool EmitVertexFetch(const Fetch& f);
  bool EmitTextureFetch(const Fetch& f);

  // Registers.
  void UseRegister(uint32_t index, bool relative) {
    if (relative) {
      info_.dynamic_registers = true;
    } else {
      max_register_ = std::max(max_register_, index + 1);
    }
  }
  std::string Reg(uint32_t index, bool relative) {
    UseRegister(index, relative);
    return relative ? Fmt("r[XE_REL(%u)]", index) : Fmt("r[%u]", index);
  }
  std::string Const(uint32_t index, bool relative, bool a0) {
    if (relative) {
      info_.dynamic_constants = true;
      if (a0) info_.relative_constant_base = std::min(info_.relative_constant_base, index);
      return a0 ? Fmt("xe_fc.c[(xe_a0 + %u) & 255]", index)
                : Fmt("xe_fc.c[(xe_aL.x + %u) & 255]", index);
    }
    info_.float_constants[index >> 5] |= 1u << (index & 31);
    return Fmt("xe_fc.c[%u]", index);
  }
  std::string AluSourceBase(const Alu& alu, int i, bool* absolute) {
    uint32_t reg = alu.src_reg(i);
    if (alu.src_is_temp(i)) {
      *absolute = (reg & 0x80) != 0;
      return Reg(reg & 0x3F, (reg & 0x40) != 0);
    }
    *absolute = alu.abs_constants();
    return Const(reg, alu.src_const_relative(i), alu.const_a0_relative());
  }
  static std::string ApplyModifiers(std::string value, bool absolute, bool negate) {
    if (absolute) value = "abs(" + value + ")";
    if (negate) value = "-" + value;
    return value;
  }
  // Full 4-component operand (vector ops).
  std::string AluVectorSource(const Alu& alu, int i, std::string* text) {
    bool absolute = false;
    std::string base = AluSourceBase(alu, i, &absolute);
    uint32_t swizzle = alu.src_swizzle(i);
    std::string sw;
    for (uint32_t c = 0; c < 4; ++c) sw += kComp[((swizzle >> (2 * c)) + c) & 3];
    std::string value = base;
    if (sw != "xyzw") value += "." + sw;
    *text = (alu.src_negate(i) ? "-" : "") + std::string(absolute ? "|" : "") + base +
            (absolute ? "|" : "") + (sw != "xyzw" ? "." + sw : "");
    return ApplyModifiers(value, absolute, alu.src_negate(i));
  }
  // One component of src3 for scalar ops: `which` = 3 for a (W), 0 for b (X).
  std::string AluScalarSource(const Alu& alu, uint32_t which) {
    bool absolute = false;
    std::string base = AluSourceBase(alu, 3, &absolute);
    uint32_t swizzle = alu.src_swizzle(3);
    char c = kComp[((swizzle >> (2 * which)) + which) & 3];
    return ApplyModifiers(base + "." + c, absolute, alu.src_negate(3));
  }

  ShaderStage stage_;
  const uint32_t* ucode_;
  size_t count_;
  TranslateOptions options_;
  std::string error_;
  std::string body_;
  std::string disasm_;
  int indent_ = 0;
  ShaderInfo info_;
  uint32_t cf_bound_ = 0;  // number of CF pairs
  std::set<uint32_t> labels_;
  uint32_t max_register_ = 0;
  bool has_vfetch_full_ = false;
  uint32_t last_vf_constant_ = 0;
  bool uses_call_ = false;
  bool uses_ps_ = false;
  std::set<uint32_t> vertex_formats_;
};

bool Translator::Scan() {
  if (count_ < 3) return Fail("microcode too short");
  cf_bound_ = uint32_t(count_ / 3);
  for (uint32_t i = 0; i < cf_bound_; ++i) {
    Cf pair[2];
    UnpackCfPair(ucode_ + i * 3, pair);
    for (const Cf& cf : pair) {
      if (cf.is_exec()) cf_bound_ = std::min(cf_bound_, cf.exec_address());
      switch (cf.op()) {
        case kCfCondCall:
          labels_.insert(cf.jump_address());
          uses_call_ = true;
          break;
        case kCfCondJmp:
          labels_.insert(cf.jump_address());
          break;
        case kCfLoopStart:
        case kCfLoopEnd:
          labels_.insert(cf.loop_address());
          break;
        default:
          break;
      }
    }
  }
  if (uses_call_) {
    // Return addresses: the instruction after each call.
    for (uint32_t i = 0; i < cf_bound_; ++i) {
      Cf pair[2];
      UnpackCfPair(ucode_ + i * 3, pair);
      for (uint32_t j = 0; j < 2; ++j) {
        if (pair[j].op() == kCfCondCall) labels_.insert(i * 2 + j + 1);
      }
    }
  }
  info_.has_jumps = !labels_.empty();
  return true;
}

bool Translator::Run(TranslateResult& out) {
  info_.stage = stage_;
  info_.ucode_hash = HashMicrocode(ucode_, count_);
  info_.ucode_dwords = uint32_t(count_);
  if (!Scan()) {
    out.error = error_;
    return false;
  }

  // Body: the guest program as xe_program().
  indent_ = 1;
  if (info_.has_jumps) {
    Line("int xe_pc = 0;");
    Line("for (int xe_guard = 0; xe_guard < 65536; ++xe_guard) {");
    ++indent_;
    Line("switch (xe_pc) {");
    ++indent_;
    Line("case 0:");
  }
  bool ok = true;
  for (uint32_t i = 0; i < cf_bound_ && ok; ++i) {
    Cf pair[2];
    UnpackCfPair(ucode_ + i * 3, pair);
    for (uint32_t j = 0; j < 2 && ok; ++j) {
      uint32_t cf_index = i * 2 + j;
      if (cf_index && labels_.count(cf_index)) {
        --indent_;
        Line(Fmt("case %u:", cf_index));
        ++indent_;
      }
      ok = EmitControlFlow(cf_index, pair[j]);
      ++info_.cf_count;
    }
  }
  if (!ok) {
    out.error = error_;
    return false;
  }
  if (info_.has_jumps) {
    // Labels past the end of the CF program end the shader.
    for (uint32_t label : labels_) {
      if (label >= cf_bound_ * 2) {
        --indent_;
        Line(Fmt("case %u:", label));
        ++indent_;
      }
    }
    Line("return;");
    --indent_;
    Line("default:");
    Line("  return;");
    --indent_;
    Line("}");
    --indent_;
    Line("}");
  }

  // Assemble the module.
  info_.register_count = info_.dynamic_registers ? 64 : std::max(max_register_, 1u);
  info_.vertex_formats.assign(vertex_formats_.begin(), vertex_formats_.end());
  const uint32_t regs = info_.register_count;
  const uint32_t interpolator_inputs = vs() ? 0 : std::min(regs, 16u);

  std::string g;
  g += "#version 450\n";
  g += Fmt("// Amped 3 native renderer: translated Xenos %s shader %016llX\n",
           vs() ? "vertex" : "pixel", (unsigned long long)info_.ucode_hash);
  g += "// Generated by src/native/xenos_shader.cpp - do not edit.\n";
  if (info_.kills) g += "#extension GL_EXT_demote_to_helper_invocation : require\n";
  g += "\n";
  // Set 0: constants (docs/native-shaders.md).
  g += "layout(set = 0, binding = 0, std140) uniform XeSystem {\n"
       "  vec4 ndc_scale;       // xyz, applied to the exported position\n"
       "  vec4 ndc_offset;      // xyz, times w\n"
       "  uint flags;           // XE_FLAG_*\n"
       "  uint vertex_index_offset;\n"
       "  uint vertex_index_mask;\n"
       "  uint index_load_address;  // bytes, when XE_FLAG_LOAD_INDEX\n"
       "  uint alpha_func;      // 0..7 (never .. always)\n"
       "  float alpha_ref;\n"
       "  uint param_gen_register;  // PS: register receiving ps_param_gen, >= 16 = off\n"
       "  float point_size;\n"
       "  float res_scale;     // internal resolution scale (rendered images)\n"
       "  uint scaled_textures;  // bit per binding (VS 0..15, PS 16..31): a scaled image\n"
       "  vec2 rt_inv_size;     // 1 / render target size in pixels\n"
       "  vec4 tex_scale[8];    // per binding (VS 0..15, PS 16..31): its image's scale\n"
       "} xe_sys;\n";
  g += "const uint XE_FLAG_XY_DIVIDED = 1u;      // position.xy already divided by w\n"
       "const uint XE_FLAG_Z_DIVIDED = 2u;       // position.z already divided by w\n"
       "const uint XE_FLAG_W_RECIPROCAL = 4u;    // position.w holds 1/w\n"
       "const uint XE_FLAG_LOAD_INDEX = 8u;      // fetch indices from guest memory\n"
       "const uint XE_FLAG_INDEX_32BIT = 16u;\n"
       "const uint XE_FLAG_INDEX_ENDIAN_SHIFT = 5u;  // 2 bits\n"
       "const uint XE_FLAG_POINT = 128u;         // PS: point primitive (param gen)\n"
       "const uint XE_FLAG_ALPHA_TO_COVERAGE = 256u;  // PS: RB_COLORCONTROL alpha_to_mask\n"
       "const uint XE_FLAG_DEBUG_STORE = 512u;  // VS: store positions (diagnostics)\n"
       "const uint XE_FLAG_SMOOTH_COVERAGE = 8192u;  // PS: alpha test / to coverage as a\n"
       "                                             // sample mask (count in bits 16..20)\n"
       "const uint XE_FLAG_CAMERA_FADE = 536870912u;  // PS: with smooth coverage, fade\n"
       "                                             // near, central cut-outs (foliage)\n"
       "const float XE_FADE_NEAR = 6.0, XE_FADE_FAR = 16.0;  // view depth (m): faded .. kept\n"
       "const float XE_FADE_R0 = 0.3, XE_FADE_R1 = 0.75;  // screen radius (heights): faded .. kept\n"
       "const float XE_FADE_MAX = 0.85;  // most a surface fades\n"
       "const float XE_SOFT_NEAR = 20.0, XE_SOFT_FAR = 40.0;  // view depth (m): ramp .. cut\n"
       "const float XE_SOFT_BELOW = 0.35, XE_SOFT_ABOVE = 0.15;  // the ramp, around the cut-off\n\n";
  // Guest texel size of a texture: rendered images are larger by their own
  // scale (the scene's or the output's, which can differ from the target's).
  g += vs() ? "float xe_tscale(uint b) { return xe_sys.tex_scale[b >> 2u][b & 3u]; }\n\n"
            : "float xe_tscale(uint b) { uint i = b + 16u; return xe_sys.tex_scale[i >> 2u][i & 3u]; "
              "}\n\n";
  g += Fmt("layout(set = 0, binding = %d, std140) uniform XeFloatConstants { vec4 c[256]; } "
           "xe_fc;\n",
           vs() ? 1 : 2);
  if (options_.velocity && vs()) {
    g += "layout(set = 0, binding = 2, std140) uniform XePrevFloatConstants { vec4 c[256]; } "
         "xe_pfc;\n";
  }
  g += "layout(set = 0, binding = 3, std140) uniform XeBoolLoopConstants {\n"
       "  uvec4 b[2];  // b0..b255\n"
       "  uvec4 l[8];  // i0..i31\n"
       "} xe_bl;\n";
  g += "layout(set = 0, binding = 4, std140) uniform XeFetchConstants { uvec4 f[48]; } "
       "xe_fetch;\n";
  if (vs()) {
    g += "layout(set = 1, binding = 0, std430) readonly buffer XeMemory { uint d[]; } xe_mem;\n";
    if (options_.debug_vertex_stores && !options_.velocity) {
      g += "layout(set = 1, binding = 1, std430) buffer XeDebug { vec4 v[]; } xe_debug;\n";
    }
  }
  const int tex_set = vs() ? 2 : 3;
  for (const TextureBinding& t : info_.textures) {
    static const char* const kTypes[4] = {"sampler2D", "sampler2D", "sampler3D", "samplerCube"};
    g += Fmt("layout(set = %d, binding = %u) uniform %s xe_tex%u;  // tf%u\n", tex_set, t.binding,
             kTypes[t.dimension & 3], t.binding, t.fetch_constant);
  }
  g += "\n";
  // Stage interface.
  if (vs()) {
    if (options_.velocity) {
      g += "layout(location = 0) out vec4 xe_vcur;\nlayout(location = 1) out vec4 xe_vprev;\n";
    } else {
      for (uint32_t i = 0; i < 16; ++i) {
        g += Fmt("layout(location = %u) out vec4 xe_out%u;\n", i, i);
      }
    }
    // Multipass effects (the board trails' stencil and depth-EQUAL passes)
    // need identical depth from every pipeline using this shader; drivers
    // may otherwise optimise each pipeline differently.
    g += "invariant gl_Position;\n";
  } else {
    for (uint32_t i = 0; i < interpolator_inputs; ++i) {
      g += Fmt("layout(location = %u) in vec4 xe_in%u;\n", i, i);
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (info_.colors_written & (1u << i)) {
        g += Fmt("layout(location = %u) out vec4 xe_color%u;\n", i, i);
      }
    }
  }
  g += "\n";
  // Guest state.
  g += Fmt("vec4 r[%u];\n", regs);
  g += Fmt("#define XE_REL(i) clamp(xe_aL.x + int(i), 0, %u)\n", regs - 1);
  g += "bool xe_p0 = false;\n"
       "int xe_a0 = 0;\n"
       "ivec4 xe_aL = ivec4(0);   // loop index stack, x = current\n"
       "uvec4 xe_lc = uvec4(0);   // loop count stack\n"
       "float xe_ps = 0.0;        // previous scalar result\n"
       "precise vec4 xe_v;\n"
       "precise float xe_s;\n"
       "vec4 xe_t0, xe_t1, xe_t2;\n"
       "float xe_sa, xe_sb;\n";
  if (uses_call_) g += "int xe_cs[4]; int xe_csp = 0;\n";
  if (vs()) {
    g += "int xe_vfa = 0;          // vfetch_full address in dwords\n"
         "vec4 xe_pos = vec4(0.0, 0.0, 0.0, 1.0);\n"
         "vec4 xe_misc = vec4(-1.0, 0.0, 0.0, 0.0);  // point size, edge flag, kill vertex\n"
         "vec4 xe_o[16];\n";
  } else {
    g += "vec4 xe_oC[4];\n"
         "float xe_depth = 0.0;\n"
         "float xe_tf_lod = 0.0;\n"
         "vec3 xe_tf_gh = vec3(0.0), xe_tf_gv = vec3(0.0);\n";
  }
  if (vs()) g += "float xe_tf_lod = 0.0;\nvec3 xe_tf_gh = vec3(0.0), xe_tf_gv = vec3(0.0);\n";
  g += "\n";
  // Helpers.
  g += R"(// Direct3D 9 multiplication: 0 * anything (including inf / NaN) = 0.
float xe_mul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xe_mul4(vec4 a, vec4 b) {
  bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
  return mix(a * b, vec4(0.0), z);
}
float xe_dp4(vec4 a, vec4 b) { vec4 m = xe_mul4(a, b); return (m.x + m.y) + (m.z + m.w); }
float xe_dp3(vec4 a, vec4 b) { vec4 m = xe_mul4(a, b); return (m.x + m.y) + m.z; }
vec4 xe_max(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float xe_clamp_inf(float t, float pos, float neg) { return isinf(t) ? (t > 0.0 ? pos : neg) : t; }
bool xe_bool(uint i) { return ((xe_bl.b[i >> 7u][(i >> 5u) & 3u] >> (i & 31u)) & 1u) != 0u; }
uint xe_loop(uint i) { return xe_bl.l[i >> 2u][i & 3u]; }
uint xe_fetch_dword(uint i) { return xe_fetch.f[i >> 2u][i & 3u]; }
vec4 xe_cube(vec4 s) {
  // s is the source with the .zzxy swizzle already applied: x = z, z = x, w = y.
  float x = s.z, y = s.w, z = s.x;
  vec3 a = abs(vec3(x, y, z));
  if (a.z >= a.x && a.z >= a.y) {
    return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);
  } else if (a.y >= a.x) {
    return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);
  }
  return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);
}
vec3 xe_cube_dir(vec3 c) {
  // tfetchCube coordinates (sc, tc in [1, 2), face) -> direction.
  float s = (c.x - 1.5) * 2.0, t = (c.y - 1.5) * 2.0;
  int face = int(clamp(floor(c.z + 0.5), 0.0, 5.0));
  if (face == 0) return vec3(1.0, -t, -s);
  if (face == 1) return vec3(-1.0, -t, s);
  if (face == 2) return vec3(s, 1.0, t);
  if (face == 3) return vec3(s, -1.0, -t);
  if (face == 4) return vec3(s, -t, 1.0);
  return vec3(-s, -t, -1.0);
}
)";
  if (vs()) {
    // Guest memory is big-endian; `endian` is the fetch constant's swap mode
    // (1 8-in-16, 2 8-in-32, 3 16-in-32).
    switch (options_.swap_variant) {
      case 1:
        g += R"(uint xe_swap(uint v, uint endian) {
  uint s8 = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
  uint a = (endian == 1u || endian == 2u) ? s8 : v;
  uint r = (a << 16u) | (a >> 16u);
  return (endian >= 2u) ? r : a;
}
)";
        break;
      case 2:
        g += R"(uint xe_swap(uint v, uint endian) {
  uint b0 = bitfieldExtract(v, 0, 8), b1 = bitfieldExtract(v, 8, 8);
  uint b2 = bitfieldExtract(v, 16, 8), b3 = bitfieldExtract(v, 24, 8);
  if (endian == 1u) return b1 | (b0 << 8u) | (b3 << 16u) | (b2 << 24u);
  if (endian == 2u) return b3 | (b2 << 8u) | (b1 << 16u) | (b0 << 24u);
  if (endian == 3u) return b2 | (b3 << 8u) | (b0 << 16u) | (b1 << 24u);
  return v;
}
)";
        break;
      case 3:
        g += R"(uint xe_swap(uint v, uint endian) {
  uvec4 b = (uvec4(v) >> uvec4(0u, 8u, 16u, 24u)) & uvec4(0xFFu);
  if (endian == 1u) return b.y | (b.x << 8u) | (b.w << 16u) | (b.z << 24u);
  if (endian == 2u) return b.w | (b.z << 8u) | (b.y << 16u) | (b.x << 24u);
  if (endian == 3u) return b.z | (b.w << 8u) | (b.x << 16u) | (b.y << 24u);
  return v;
}
)";
        break;
      default:
        g += R"(uint xe_swap(uint v, uint endian) {
  if (endian == 1u) return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
  if (endian == 2u) {
    v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    return (v << 16u) | (v >> 16u);
  }
  if (endian == 3u) return (v << 16u) | (v >> 16u);
  return v;
}
)";
        break;
    }
    g += R"(uint xe_vword(uint vf, int address) {
  return xe_swap(xe_mem.d[uint(max(address, 0))], xe_fetch_dword(vf * 2u + 1u) & 3u);
}
float xe_unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float xe_snorm(int v, uint bits, bool no_zero) {
  float scale = float((1u << (bits - 1u)) - 1u);
  return no_zero ? (float(v) + 0.5) / (scale + 0.5) : max(float(v) / scale, -1.0);
}
uint xe_vertex_index() {
  uint index = uint(gl_VertexIndex);
  if ((xe_sys.flags & XE_FLAG_LOAD_INDEX) != 0u) {
    uint endian = (xe_sys.flags >> XE_FLAG_INDEX_ENDIAN_SHIFT) & 3u;
    if ((xe_sys.flags & XE_FLAG_INDEX_32BIT) != 0u) {
      index = xe_swap(xe_mem.d[((xe_sys.index_load_address & 0x1FFFFFFFu) >> 2u) + index], endian);
    } else {
      uint address = (xe_sys.index_load_address & 0x1FFFFFFFu) + index * 2u;
      uint word = xe_swap(xe_mem.d[address >> 2u], endian);
      index = (word >> ((address & 2u) * 8u)) & 0xFFFFu;
    }
  }
  return (index + xe_sys.vertex_index_offset) & xe_sys.vertex_index_mask;
}
)";
  }
  g += "\n";
  g += "void xe_program() {\n";
  g += body_;
  g += "}\n\n";
  if (options_.velocity && vs()) {
    // The same program reading last frame's constants.
    std::string prev = body_;
    for (size_t at = 0; (at = prev.find("xe_fc.", at)) != std::string::npos; at += 7) {
      prev.replace(at, 6, "xe_pfc.");
    }
    // Its own precise temporaries: glslang emits invalid SPIR-V (a value used
    // where it isn't defined) when two functions share a precise global.
    prev = std::regex_replace(prev, std::regex(R"(\bxe_v\b)"), "xe_pv");
    prev = std::regex_replace(prev, std::regex(R"(\bxe_s\b)"), "xe_pscalar");
    g += "precise vec4 xe_pv;\nprecise float xe_pscalar;\n\n";
    g += "void xe_program_prev() {\n";
    g += prev;
    g += "}\n\n";
    g += "vec4 xe_clip() {\n"
         "  precise vec4 p = xe_pos;\n"
         "  if ((xe_sys.flags & XE_FLAG_W_RECIPROCAL) != 0u) p.w = 1.0 / p.w;\n"
         "  if ((xe_sys.flags & XE_FLAG_XY_DIVIDED) != 0u) p.xy *= p.w;\n"
         "  if ((xe_sys.flags & XE_FLAG_Z_DIVIDED) != 0u) p.z *= p.w;\n"
         "  p.xyz = p.xyz * xe_sys.ndc_scale.xyz + xe_sys.ndc_offset.xyz * p.w;\n"
         "  return p;\n"
         "}\n\n";
    g += "void xe_reset() {\n";
    g += Fmt("  for (int i = 0; i < %u; ++i) r[i] = vec4(0.0);\n", regs);
    g += "  for (int i = 0; i < 16; ++i) xe_o[i] = vec4(0.0);\n"
         "  xe_p0 = false; xe_a0 = 0; xe_aL = ivec4(0); xe_lc = uvec4(0); xe_ps = 0.0;\n"
         "  xe_vfa = 0; xe_pos = vec4(0.0, 0.0, 0.0, 1.0); xe_misc = vec4(-1.0, 0.0, 0.0, 0.0);\n";
    if (uses_call_) g += "  xe_csp = 0;\n";
    g += "  r[0].x = float(xe_vertex_index());\n"
         "}\n\n";
    g += "void main() {\n"
         "  xe_reset();\n"
         "  xe_program();\n"
         "  precise vec4 p = xe_clip();\n"
         "  gl_Position = p;\n"
         "  xe_vcur = p;\n"
         "  xe_reset();\n"
         "  xe_program_prev();\n"
         "  xe_vprev = xe_clip();\n"
         "  gl_PointSize = xe_sys.point_size * xe_sys.res_scale;\n"
         "}\n";
    out.glsl = std::move(g);
    out.disasm = std::move(disasm_);
    out.info = std::move(info_);
    out.ok = true;
    return true;
  }
  // Entry point with prologue and epilogue.
  g += "void main() {\n";
  g += Fmt("  for (int i = 0; i < %u; ++i) r[i] = vec4(0.0);\n", regs);
  if (vs()) {
    g += "  for (int i = 0; i < 16; ++i) xe_o[i] = vec4(0.0);\n";
    g += "  r[0].x = float(xe_vertex_index());\n";
    g += "  xe_program();\n";
    g += R"(  precise vec4 p = xe_pos;
  if ((xe_sys.flags & XE_FLAG_W_RECIPROCAL) != 0u) p.w = 1.0 / p.w;
  if ((xe_sys.flags & XE_FLAG_XY_DIVIDED) != 0u) p.xy *= p.w;
  if ((xe_sys.flags & XE_FLAG_Z_DIVIDED) != 0u) p.z *= p.w;
  p.xyz = p.xyz * xe_sys.ndc_scale.xyz + xe_sys.ndc_offset.xyz * p.w;
  gl_Position = p;
)";
    if (options_.debug_vertex_stores) {
      g += Fmt(R"(  if ((xe_sys.flags & XE_FLAG_DEBUG_STORE) != 0u) {
    uint dv = uint(gl_VertexIndex);
    if (dv < 4096u) {
      xe_debug.v[dv * 4u + 0u] = xe_pos;
      xe_debug.v[dv * 4u + 1u] = p;
      xe_debug.v[dv * 4u + 2u] = vec4(float(gl_VertexIndex), float(xe_vertex_index()),
                                      uintBitsToFloat(xe_fetch_dword(190u)),
                                      uintBitsToFloat(xe_fetch_dword(191u)));
      xe_debug.v[dv * 4u + 3u] = r[%u];  // the trail shader's raw position
    }
  }
)", std::min(regs - 1, 10u));
    }
    // Always written: with point topologies the size is otherwise undefined.
    if (info_.writes_point_size) {
      g += "  gl_PointSize = (xe_misc.x > 0.0 ? xe_misc.x : xe_sys.point_size) * xe_sys.res_scale;\n";
    } else {
      g += "  gl_PointSize = xe_sys.point_size * xe_sys.res_scale;\n";
    }
    for (uint32_t i = 0; i < 16; ++i) g += Fmt("  xe_out%u = xe_o[%u];\n", i, i);
  } else {
    for (uint32_t i = 0; i < interpolator_inputs; ++i) g += Fmt("  r[%u] = xe_in%u;\n", i, i);
    g += Fmt(R"(  if (xe_sys.param_gen_register < %uu) {
    // ps_param_gen: pixel position (sign of x = back face), point coordinates.
    vec2 pixel = floor(gl_FragCoord.xy / xe_sys.res_scale);
    bool point = (xe_sys.flags & XE_FLAG_POINT) != 0u;
    r[xe_sys.param_gen_register] = vec4(gl_FrontFacing ? pixel.x : -pixel.x,
                                        point ? -pixel.y : pixel.y,
                                        point ? gl_PointCoord : vec2(0.0));
  }
)",
             std::min(regs, 16u));
    g += "  for (int i = 0; i < 4; ++i) xe_oC[i] = vec4(0.0);\n";
    g += "  xe_program();\n";
    if (info_.colors_written & 1) {
      const char* kill = info_.kills ? "demote;" : "discard;";
      g += Fmt(R"(  // Multisampled: alpha test and alpha to coverage (RB_COLORCONTROL) as
  // the console does them, with smooth edges. Alpha to mask covers sample i
  // when alpha is above its threshold (alpha_to_mask_offset i: (offset +
  // 0.5) / 4); with all offsets equal (as Amped 3 sets them) that's one
  // cut-off, solid, not a fade. A single cut-off (that, or an alpha test
  // greater / greater-equal) becomes an anti-aliased edge: coverage from how
  // far the pixel is from it, in pixels, dithered between the samples'
  // levels (4x4 ordered).
  gl_SampleMask[0] = -1;
  if ((xe_sys.flags & XE_FLAG_SMOOTH_COVERAGE) != 0u) {
    float a = xe_oC[0].a;
    uint f = xe_sys.alpha_func;
    uint n = max((xe_sys.flags >> 16) & 31u, 1u);
    uint full = (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    bool to_mask = (xe_sys.flags & XE_FLAG_ALPHA_TO_COVERAGE) != 0u;
    uint offsets = (xe_sys.flags >> 21) & 0xFFu;
    float cut = -1.0;  // the alpha test's cut-off (greater / greater-equal)
    if (f == 4u || f == 6u) {
      cut = xe_sys.alpha_ref;
    } else if (f != 7u) {
      float ref = xe_sys.alpha_ref;
      bool pass = (f == 1u && a < ref) || (f == 2u && a == ref) || (f == 3u && a <= ref) ||
                  (f == 5u && a != ref);
      if (!pass) %s
    }
    bool one_cut = !to_mask || offsets == (offsets & 3u) * 0x55u;
    uvec2 q = uvec2(gl_FragCoord.xy) & 3u;
    const uint kBayer[16] = uint[16](0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u,
                                     3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);
    float dither = (float(kBayer[q.y * 4u + q.x]) + 0.5) / 16.0;
    uint rot = uint(dither * float(n)) %% n;
    // Camera fade: visibility from the pixel's distance (view depth) and its
    // distance from the screen point the rider sits around, smooth in both.
    float vis = 1.0;
    float soft = 0.0;  // near foliage: how much the cut-off becomes a ramp
    if ((xe_sys.flags & XE_FLAG_CAMERA_FADE) != 0u) {
      vec2 uv = gl_FragCoord.xy * xe_sys.rt_inv_size;
      float aspect = xe_sys.rt_inv_size.y / xe_sys.rt_inv_size.x;
      float depth = 1.0 / max(gl_FragCoord.w, 1e-6);
      float r = length((uv - vec2(0.5, 0.55)) * vec2(aspect, 1.0));
      float near_fade = 1.0 - smoothstep(XE_FADE_NEAR, XE_FADE_FAR, depth);
      float centre_fade = 1.0 - smoothstep(XE_FADE_R0, XE_FADE_R1, r);
      // Only surfaces with alpha below one: foliage (the game lowers the
      // alpha of trees near the camera); terrain and trunks share the state
      // but are opaque.
      float leafy = 1.0 - smoothstep(0.97, 0.999, a);
      vis = 1.0 - XE_FADE_MAX * near_fade * centre_fade * leafy;
      // The game fades trees near the camera by lowering their alpha, which
      // against a single cut-off eats ragged holes along the texture's alpha
      // contours. Close up, the cut-off becomes a ramp instead: coverage
      // follows alpha, so a faded tree turns evenly see-through.
      soft = 1.0 - smoothstep(XE_SOFT_NEAR, XE_SOFT_FAR, depth);
    }
    uint bits = full;
    if (one_cut) {
      float t = to_mask ? max(cut, (float(offsets & 3u) + 0.5) / 4.0) : cut;
      float cov = 1.0;
      if (t >= 0.0) {
        // The anti-aliased edge lies on the passing side of the cut-off, so
        // alpha exactly at it keeps the test's own answer: none for greater
        // (a sprite's fully transparent texels against a cut-off of 0 must
        // cover nothing, or its whole quad writes depth over what's behind),
        // all for greater-equal.
        float w = max(fwidth(a), 1e-3);
        cov = clamp((a - t) / w + (f == 6u ? 1.0 : 0.0), 0.0, 1.0);
        if (soft > 0.0) {
          float ramp = smoothstep(max(t - XE_SOFT_BELOW, 0.0), t + XE_SOFT_ABOVE, a);
          cov = mix(cov, ramp, soft);
        }
      }
      cov *= vis;
      if (cov < 1.0) {
        uint k = uint(clamp(floor(cov * float(n) + dither), 0.0, float(n)));
        bits = (k >= 32u) ? 0xFFFFFFFFu : ((1u << k) - 1u);
        bits = ((bits << rot) | (bits >> ((n - rot) %% 32u))) & full;
      }
    } else {
      // Dithered alpha to mask: each sample its own threshold (guest sample
      // i %% 4's offset).
      bits = 0u;
      for (uint i = 0u; i < n && i < 32u; ++i) {
        float t = max(cut, (float((offsets >> (2u * (i & 3u))) & 3u) + 0.5) / 4.0);
        if (a > t) bits |= 1u << i;
      }
      if (vis < 1.0) {
        uint k = uint(clamp(floor(vis * float(n) + dither), 0.0, float(n)));
        uint m = (k >= 32u) ? 0xFFFFFFFFu : ((1u << k) - 1u);
        bits &= ((m << rot) | (m >> ((n - rot) %% 32u))) & full;
      }
    }
    if (bits == 0u) %s
    gl_SampleMask[0] = int(bits);
  } else
)", kill, kill);
      g += R"(  // Alpha test (RB_COLORCONTROL).
  if (xe_sys.alpha_func != 7u) {
    float a = xe_oC[0].a, ref = xe_sys.alpha_ref;
    uint f = xe_sys.alpha_func;
    bool pass = (f == 1u && a < ref) || (f == 2u && a == ref) || (f == 3u && a <= ref) ||
                (f == 4u && a > ref) || (f == 5u && a != ref) || (f == 6u && a >= ref);
    if (!pass) )";
      g += info_.kills ? "demote;\n" : "discard;\n";
      g += "  }\n";
      // Alpha to coverage with one sample per pixel: keep the fragment when at
      // least half the samples would be covered.
      g += "  if ((xe_sys.flags & XE_FLAG_ALPHA_TO_COVERAGE) != 0u && xe_oC[0].a < 0.5) ";
      g += info_.kills ? "demote;\n" : "discard;\n";
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (info_.colors_written & (1u << i)) g += Fmt("  xe_color%u = xe_oC[%u];\n", i, i);
    }
    if (info_.writes_depth) g += "  gl_FragDepth = clamp(xe_depth, 0.0, 1.0);\n";
  }
  g += "}\n";

  out.glsl = std::move(g);
  out.disasm = std::move(disasm_);
  out.info = std::move(info_);
  out.ok = true;
  return true;
}

bool Translator::EmitControlFlow(uint32_t cf_index, const Cf& cf) {
  switch (cf.op()) {
    case kCfNop:
      return true;
    case kCfAlloc:
      Disasm(Fmt("alloc %s, %u", cf.alloc_type() == 1   ? "position"
                                 : cf.alloc_type() == 2 ? (vs() ? "interpolators" : "colors")
                                 : cf.alloc_type() == 3 ? "memory"
                                                        : "none",
                 cf.alloc_size()));
      if (cf.alloc_type() == 3) return Fail("memory export is not supported");
      return true;
    case kCfMarkVsFetchDone:
      return true;
    case kCfExec:
    case kCfExecEnd:
    case kCfCondExec:
    case kCfCondExecEnd:
    case kCfCondExecPred:
    case kCfCondExecPredEnd:
    case kCfCondExecPredClean:
    case kCfCondExecPredCleanEnd:
      return EmitExec(cf);
    case kCfLoopStart: {
      info_.uses_loop_constants = true;
      Disasm(Fmt("loop i%u, L%u%s", cf.loop_id(), cf.loop_address(),
                 cf.loop_is_repeat() ? " (repeat)" : ""));
      Line(Fmt("{ uint c = xe_loop(%uu);", cf.loop_id()));
      Line("  xe_lc = uvec4(c & 255u, xe_lc.xyz);");
      if (cf.loop_is_repeat()) {
        Line("  xe_aL = ivec4(xe_aL.x, xe_aL.xyz);");
      } else {
        Line("  xe_aL = ivec4(int((c >> 8u) & 255u), xe_aL.xyz);");
      }
      Line(Fmt("  if (xe_lc.x == 0u) { xe_pc = %u; continue; } }", cf.loop_address()));
      return true;
    }
    case kCfLoopEnd: {
      info_.uses_loop_constants = true;
      Disasm(Fmt("endloop i%u, L%u%s", cf.loop_id(), cf.loop_address(),
                 cf.loop_predicated_break() ? (cf.condition() ? " break if p0" : " break if !p0")
                                            : ""));
      std::string cont = "xe_lc.x != 0u";
      if (cf.loop_predicated_break()) {
        cont += cf.condition() ? " && !xe_p0" : " && xe_p0";
      }
      Line("xe_lc.x -= 1u;");
      Line(Fmt("if (%s) {", cont.c_str()));
      Line(Fmt("  xe_aL.x += bitfieldExtract(int(xe_loop(%uu)), 16, 8);", cf.loop_id()));
      Line(Fmt("  xe_pc = %u; continue;", cf.loop_address()));
      Line("}");
      Line("xe_lc = uvec4(xe_lc.yzw, 0u);");
      Line("xe_aL = ivec4(xe_aL.yzw, 0);");
      return true;
    }
    case kCfCondJmp:
    case kCfCondCall: {
      bool call = cf.op() == kCfCondCall;
      std::string cond;
      std::string text = Fmt("%s L%u", call ? "call" : "jmp", cf.jump_address());
      if (!cf.jump_unconditional()) {
        if (cf.jump_predicated()) {
          cond = cf.condition() ? "xe_p0" : "!xe_p0";
          text = (cf.condition() ? "(p0) " : "(!p0) ") + text;
        } else {
          info_.uses_bool_constants = true;
          cond = Fmt("%sxe_bool(%uu)", cf.condition() ? "" : "!", cf.bool_address());
          text = Fmt("c%s b%u, ", call ? "call" : "jmp", cf.bool_address()) + text +
                 (cf.condition() ? "" : " (if false)");
        }
      }
      Disasm(text);
      std::string action = call ? Fmt("if (xe_csp < 4) xe_cs[xe_csp++] = %u; xe_pc = %u; continue;",
                                      cf_index + 1, cf.jump_address())
                                : Fmt("xe_pc = %u; continue;", cf.jump_address());
      if (cond.empty()) {
        Line(action);
      } else {
        Line(Fmt("if (%s) { %s }", cond.c_str(), action.c_str()));
      }
      return true;
    }
    case kCfReturn:
      Disasm("ret");
      if (uses_call_) {
        Line("if (xe_csp > 0) { xe_pc = xe_cs[--xe_csp]; continue; }");
      }
      return true;
    default:
      return Fail(Fmt("unknown control flow opcode %u", cf.op()));
  }
}

bool Translator::EmitExec(const Cf& cf) {
  std::string cond;
  std::string name;
  switch (cf.op()) {
    case kCfExec: name = "exec"; break;
    case kCfExecEnd: name = "exece"; break;
    case kCfCondExec:
    case kCfCondExecEnd:
    case kCfCondExecPredClean:
    case kCfCondExecPredCleanEnd:
      info_.uses_bool_constants = true;
      cond = Fmt("%sxe_bool(%uu)", cf.condition() ? "" : "!", cf.bool_address());
      name = Fmt("cexec%s %sb%u", cf.is_end() ? "e" : "", cf.condition() ? "" : "!",
                 cf.bool_address());
      break;
    case kCfCondExecPred:
    case kCfCondExecPredEnd:
      cond = cf.condition() ? "xe_p0" : "!xe_p0";
      name = Fmt("(%sp0) exec%s", cf.condition() ? "" : "!", cf.is_end() ? "e" : "");
      break;
  }
  Disasm(Fmt("%s @%u x%u", name.c_str(), cf.exec_address(), cf.exec_count()));
  if (!cond.empty()) {
    Line(Fmt("if (%s) {", cond.c_str()));
    ++indent_;
  }
  uint32_t sequence = cf.exec_sequence();
  for (uint32_t k = 0; k < cf.exec_count(); ++k, sequence >>= 2) {
    uint32_t address = cf.exec_address() + k;
    if (size_t(address) * 3 + 3 > count_) return Fail("exec reads past the end of the microcode");
    const uint32_t* d = ucode_ + size_t(address) * 3;
    bool ok;
    if (sequence & 1) {
      Fetch f{{d[0], d[1], d[2]}};
      ok = f.op() == kVertexFetch ? EmitVertexFetch(f) : EmitTextureFetch(f);
    } else {
      Alu a{{d[0], d[1], d[2]}};
      ok = EmitAlu(a);
    }
    if (!ok) return false;
  }
  if (cf.is_end()) Line("return;");
  if (!cond.empty()) {
    --indent_;
    Line("}");
  }
  return true;
}

// --- ALU -------------------------------------------------------------------------

bool Translator::EmitAlu(const Alu& alu) {
  ++info_.alu_count;
  const uint32_t vop = alu.vector_op();
  const uint32_t sop = alu.scalar_op();
  const ScalarInfo& sinfo = kScalarInfo[sop];
  if (vop >= 30) return Fail(Fmt("unknown vector opcode %u", vop));
  if (sop == kOpcode41 || sop > kRetainPrev) return Fail(Fmt("unknown scalar opcode %u", sop));

  // Destinations.
  const bool is_export = alu.is_export();
  const uint32_t vmask = alu.vector_result_mask();
  const uint32_t smask = alu.scalar_result_mask();
  const uint32_t c0mask = alu.const0_mask();
  const uint32_t c1mask = alu.const1_mask();
  std::string export_target;
  uint32_t export_used_mask = 0xF;
  if (is_export) {
    uint32_t reg = alu.vector_dest();
    if (vs()) {
      if (reg < 16) {
        export_target = Fmt("xe_o[%u]", reg);
        info_.interpolators_written |= 1u << reg;
      } else if (reg == kExportPosition) {
        export_target = "xe_pos";
      } else if (reg == kExportPointSize) {
        export_target = "xe_misc";
        if ((vmask | smask | c0mask | c1mask) & 1) info_.writes_point_size = true;
      }
    } else {
      if (reg < 4) {
        export_target = Fmt("xe_oC[%u]", reg);
        if (vmask | smask | c0mask | c1mask) info_.colors_written |= 1u << reg;
      } else if (reg == kExportDepth) {
        export_target = "xe_depth";
        export_used_mask = 1;
        if ((vmask | smask | c0mask | c1mask) & 1) info_.writes_depth = true;
      }
    }
    if (reg == kExportAddress || (reg > kExportAddress && reg <= kExportAddress + 5)) {
      return Fail("memory export is not supported");
    }
    if (export_target.empty()) return Fail(Fmt("unsupported export register %u", reg));
  }

  // Whether each operation has to run.
  const bool vector_state = (vop >= kSetpEqPush && vop <= kKillNe) || vop == kMaxA;
  const bool run_vector = vmask != 0 || vector_state;
  // Scalar ops always update ps (except retain_prev), which later ops may read.
  const bool run_scalar = !(sop == kRetainPrev && smask == 0);

  // Disassembly.
  std::string vtext, stext;
  {
    std::string dest = is_export ? export_target
                                 : Fmt("r%u%s", alu.vector_dest(),
                                       alu.vector_dest_rel() ? "[aL]" : "");
    std::string mask;
    for (uint32_t c = 0; c < 4; ++c) {
      mask += (alu.vector_mask() & (1u << c)) ? kComp[c] : '_';
    }
    vtext = Fmt("%s%s %s.%s", kVectorNames[vop], alu.vector_clamp() ? "_sat" : "", dest.c_str(),
                mask.c_str());
    stext = Fmt("%s%s ", sinfo.name, alu.scalar_clamp() ? "_sat" : "");
    if (is_export) {
      stext += export_target;
    } else {
      stext += Fmt("r%u%s", alu.scalar_dest(), alu.scalar_dest_rel() ? "[aL]" : "");
    }
    mask.clear();
    for (uint32_t c = 0; c < 4; ++c) {
      mask += (alu.scalar_mask() & (1u << c)) ? kComp[c] : '_';
    }
    stext += "." + mask;
  }

  // Operands (all read before anything is written).
  std::vector<std::string> loads;
  std::string vsrc_text;
  if (run_vector) {
    for (int i = 1; i <= kVectorOperands[vop]; ++i) {
      std::string text;
      std::string value = AluVectorSource(alu, i, &text);
      vsrc_text += ", " + text;
      loads.push_back(Fmt("xe_t%d = %s;", i - 1, value.c_str()));
    }
  }
  std::string ssrc_text;
  if (sinfo.operands == 1) {
    std::string a = AluScalarSource(alu, 3);
    loads.push_back("xe_sa = " + a + ";");
    ssrc_text = ", " + a;
    if (sinfo.two_component) {
      std::string b = AluScalarSource(alu, 0);
      loads.push_back("xe_sb = " + b + ";");
      ssrc_text += ", " + b;
    }
  } else if (sinfo.operands == 2) {
    // Constant (a, W swizzle) and temporary (b, X swizzle); abs / negate on both.
    uint32_t swizzle = alu.src_swizzle(3);
    char ca = kComp[((swizzle >> 6) + 3) & 3];
    char cb = kComp[(swizzle + 0) & 3];
    std::string a = Const(alu.src_reg(3), alu.src_const_relative(3), alu.const_a0_relative()) +
                    "." + ca;
    std::string b = Reg(alu.scalar_const_op_temp_reg(), false) + "." + cb;
    a = ApplyModifiers(a, alu.abs_constants(), alu.src_negate(3));
    b = ApplyModifiers(b, alu.abs_constants(), alu.src_negate(3));
    loads.push_back("xe_sa = " + a + ";");
    loads.push_back("xe_sb = " + b + ";");
    ssrc_text = ", " + a + ", " + b;
  }
  Disasm((alu.predicated() ? (alu.pred_condition() ? "(p0) " : "(!p0) ") : std::string()) +
         vtext + vsrc_text + "  +  " + stext + ssrc_text);
  if (alu.predicated()) {
    Line(Fmt("if (%sxe_p0) {", alu.pred_condition() ? "" : "!"));
    ++indent_;
  }
  for (const std::string& l : loads) Line(l);

  // Vector operation.
  if (run_vector) {
    std::string e;
    switch (vop) {
      case kAdd: e = "xe_t0 + xe_t1"; break;
      case kMul: e = "xe_mul4(xe_t0, xe_t1)"; break;
      case kMax: e = "xe_max(xe_t0, xe_t1)"; break;
      case kMin: e = "xe_min(xe_t0, xe_t1)"; break;
      case kSeq: e = "vec4(equal(xe_t0, xe_t1))"; break;
      case kSgt: e = "vec4(greaterThan(xe_t0, xe_t1))"; break;
      case kSge: e = "vec4(greaterThanEqual(xe_t0, xe_t1))"; break;
      case kSne: e = "vec4(notEqual(xe_t0, xe_t1))"; break;
      case kFrc: e = "fract(xe_t0)"; break;
      case kTrunc: e = "trunc(xe_t0)"; break;
      case kFloor: e = "floor(xe_t0)"; break;
      case kMad: e = "xe_mul4(xe_t0, xe_t1) + xe_t2"; break;
      case kCndEq: e = "mix(xe_t2, xe_t1, equal(xe_t0, vec4(0.0)))"; break;
      case kCndGe: e = "mix(xe_t2, xe_t1, greaterThanEqual(xe_t0, vec4(0.0)))"; break;
      case kCndGt: e = "mix(xe_t2, xe_t1, greaterThan(xe_t0, vec4(0.0)))"; break;
      case kDp4: e = "vec4(xe_dp4(xe_t0, xe_t1))"; break;
      case kDp3: e = "vec4(xe_dp3(xe_t0, xe_t1))"; break;
      case kDp2Add:
        e = "vec4((xe_mul(xe_t0.x, xe_t1.x) + xe_mul(xe_t0.y, xe_t1.y)) + xe_t2.x)";
        break;
      case kCube: e = "xe_cube(xe_t0)"; break;
      case kMax4:
        e = "vec4(max(max(xe_t0.x, xe_t0.y), max(xe_t0.z, xe_t0.w)))";
        break;
      case kSetpEqPush:
      case kSetpNePush:
      case kSetpGtPush:
      case kSetpGePush: {
        static const char* const kCmp[4] = {"==", "!=", ">", ">="};
        const char* cmp = kCmp[vop - kSetpEqPush];
        Line(Fmt("xe_p0 = xe_t0.w == 0.0 && xe_t1.w %s 0.0;", cmp));
        e = Fmt("vec4((xe_t0.x == 0.0 && xe_t1.x %s 0.0) ? 0.0 : xe_t0.x + 1.0)", cmp);
        break;
      }
      case kKillEq:
      case kKillGt:
      case kKillGe:
      case kKillNe: {
        if (vs()) return Fail("kill in a vertex shader");
        static const char* const kCmp[4] = {"equal", "greaterThan", "greaterThanEqual",
                                            "notEqual"};
        info_.kills = true;
        Line(Fmt("if (any(%s(xe_t0, xe_t1))) { demote; xe_v = vec4(1.0); } "
                 "else { xe_v = vec4(0.0); }",
                 kCmp[vop - kKillEq]));
        break;
      }
      case kDst: e = "vec4(1.0, xe_mul(xe_t0.y, xe_t1.y), xe_t0.z, xe_t1.w)"; break;
      case kMaxA:
        Line("xe_a0 = int(clamp(floor(xe_t0.w + 0.5), -256.0, 255.0));");
        e = "xe_max(xe_t0, xe_t1)";
        break;
    }
    if (!e.empty()) Line("xe_v = " + e + ";");
  }

  // Scalar operation.
  if (run_scalar) {
    std::string e;
    bool sets_ps = true;
    switch (sop) {
      case kAdds: e = "xe_sa + xe_sb"; break;
      case kAddsPrev: e = "xe_sa + xe_ps"; uses_ps_ = true; break;
      case kMuls: e = "xe_mul(xe_sa, xe_sb)"; break;
      case kMulsPrev: e = "xe_mul(xe_sa, xe_ps)"; uses_ps_ = true; break;
      case kMulsPrev2:
        e = Fmt("(xe_ps == -%s || isinf(xe_ps) || isnan(xe_ps) || isinf(xe_sb) || isnan(xe_sb) "
                "|| xe_sb <= 0.0) ? -%s : xe_mul(xe_sa, xe_ps)",
                kFltMax, kFltMax);
        uses_ps_ = true;
        break;
      case kMaxs: e = "xe_sa >= xe_sb ? xe_sa : xe_sb"; break;
      case kMins: e = "xe_sa < xe_sb ? xe_sa : xe_sb"; break;
      case kSeqs: e = "xe_sa == 0.0 ? 1.0 : 0.0"; break;
      case kSgts: e = "xe_sa > 0.0 ? 1.0 : 0.0"; break;
      case kSges: e = "xe_sa >= 0.0 ? 1.0 : 0.0"; break;
      case kSnes: e = "xe_sa != 0.0 ? 1.0 : 0.0"; break;
      case kFrcs: e = "fract(xe_sa)"; break;
      case kTruncs: e = "trunc(xe_sa)"; break;
      case kFloors: e = "floor(xe_sa)"; break;
      case kExp: e = "exp2(xe_sa)"; break;
      case kLogc: e = Fmt("xe_clamp_inf(log2(xe_sa), %s, -%s)", kFltMax, kFltMax); break;
      case kLog: e = "log2(xe_sa)"; break;
      case kRcpc: e = Fmt("xe_clamp_inf(1.0 / xe_sa, %s, -%s)", kFltMax, kFltMax); break;
      case kRcpf: e = "xe_clamp_inf(1.0 / xe_sa, 0.0, -0.0)"; break;
      case kRcp: e = "1.0 / xe_sa"; break;
      case kRsqc: e = Fmt("xe_clamp_inf(inversesqrt(xe_sa), %s, -%s)", kFltMax, kFltMax); break;
      case kRsqf: e = "xe_clamp_inf(inversesqrt(xe_sa), 0.0, -0.0)"; break;
      case kRsq: e = "inversesqrt(xe_sa)"; break;
      case kMaxAs:
        Line("xe_a0 = int(clamp(floor(xe_sa + 0.5), -256.0, 255.0));");
        e = "xe_sa >= xe_sb ? xe_sa : xe_sb";
        break;
      case kMaxAsf:
        Line("xe_a0 = int(clamp(floor(xe_sa), -256.0, 255.0));");
        e = "xe_sa >= xe_sb ? xe_sa : xe_sb";
        break;
      case kSubs: e = "xe_sa - xe_sb"; break;
      case kSubsPrev: e = "xe_sa - xe_ps"; uses_ps_ = true; break;
      case kSetpEq: Line("xe_p0 = xe_sa == 0.0;"); e = "xe_p0 ? 0.0 : 1.0"; break;
      case kSetpNe: Line("xe_p0 = xe_sa != 0.0;"); e = "xe_p0 ? 0.0 : 1.0"; break;
      case kSetpGt: Line("xe_p0 = xe_sa > 0.0;"); e = "xe_p0 ? 0.0 : 1.0"; break;
      case kSetpGe: Line("xe_p0 = xe_sa >= 0.0;"); e = "xe_p0 ? 0.0 : 1.0"; break;
      case kSetpInv:
        Line("xe_p0 = xe_sa == 1.0;");
        e = "xe_p0 ? 0.0 : (xe_sa == 0.0 ? 1.0 : xe_sa)";
        break;
      case kSetpPop:
        Line("xe_p0 = xe_sa - 1.0 <= 0.0;");
        e = "xe_p0 ? 0.0 : xe_sa - 1.0";
        break;
      case kSetpClr:
        Line("xe_p0 = false;");
        e = kFltMax;
        break;
      case kSetpRstr:
        Line("xe_p0 = xe_sa == 0.0;");
        e = "xe_p0 ? 0.0 : xe_sa";
        break;
      case kKillsEq:
      case kKillsGt:
      case kKillsGe:
      case kKillsNe:
      case kKillsOne: {
        if (vs()) return Fail("kill in a vertex shader");
        static const char* const kCond[5] = {"xe_sa == 0.0", "xe_sa > 0.0", "xe_sa >= 0.0",
                                             "xe_sa != 0.0", "xe_sa == 1.0"};
        info_.kills = true;
        Line(Fmt("if (%s) { demote; xe_s = 1.0; } else { xe_s = 0.0; }", kCond[sop - kKillsEq]));
        break;
      }
      case kSqrt: e = "sqrt(xe_sa)"; break;
      case kMulsc0:
      case kMulsc1: e = "xe_mul(xe_sa, xe_sb)"; break;
      case kAddsc0:
      case kAddsc1: e = "xe_sa + xe_sb"; break;
      case kSubsc0:
      case kSubsc1: e = "xe_sa - xe_sb"; break;
      case kSin: e = "sin(xe_sa)"; break;
      case kCos: e = "cos(xe_sa)"; break;
      case kRetainPrev:
        e = "xe_ps";
        sets_ps = false;
        break;
    }
    if (!e.empty()) Line("xe_s = " + e + ";");
    if (sets_ps) Line("xe_ps = xe_s;");
  }

  // Writes.
  auto mask_str = [](uint32_t mask) {
    std::string s;
    for (uint32_t c = 0; c < 4; ++c) {
      if (mask & (1u << c)) s += kComp[c];
    }
    return s;
  };
  auto clamp_expr = [](const std::string& value, bool clamp) {
    return clamp ? "clamp(" + value + ", 0.0, 1.0)" : value;
  };
  if (is_export) {
    if (export_target == "xe_depth") {
      if (vmask & 1) Line("xe_depth = " + clamp_expr("xe_v.x", alu.vector_clamp()) + ";");
      if (smask & 1) Line("xe_depth = " + clamp_expr("xe_s", alu.scalar_clamp()) + ";");
      if (c0mask & 1) Line("xe_depth = 0.0;");
      if (c1mask & 1) Line("xe_depth = 1.0;");
    } else {
      (void)export_used_mask;
      if (vmask) {
        std::string m = mask_str(vmask);
        Line(Fmt("%s.%s = %s;", export_target.c_str(), m.c_str(),
                 clamp_expr("xe_v." + m, alu.vector_clamp()).c_str()));
      }
      if (smask) {
        std::string m = mask_str(smask);
        std::string value = m.size() == 1 ? "xe_s" : Fmt("vec%zu(xe_s)", m.size());
        Line(Fmt("%s.%s = %s;", export_target.c_str(), m.c_str(),
                 clamp_expr(value, alu.scalar_clamp()).c_str()));
      }
      if (c0mask) {
        std::string m = mask_str(c0mask);
        Line(Fmt("%s.%s = %s;", export_target.c_str(), m.c_str(),
                 m.size() == 1 ? "0.0" : Fmt("vec%zu(0.0)", m.size()).c_str()));
      }
      if (c1mask) {
        std::string m = mask_str(c1mask);
        Line(Fmt("%s.%s = %s;", export_target.c_str(), m.c_str(),
                 m.size() == 1 ? "1.0" : Fmt("vec%zu(1.0)", m.size()).c_str()));
      }
    }
  } else {
    if (vmask) {
      std::string m = mask_str(vmask);
      std::string dest = Reg(alu.vector_dest(), alu.vector_dest_rel());
      Line(Fmt("%s.%s = %s;", dest.c_str(), m.c_str(),
               clamp_expr("xe_v." + m, alu.vector_clamp()).c_str()));
    }
    if (smask) {
      std::string m = mask_str(smask);
      std::string dest = Reg(alu.scalar_dest(), alu.scalar_dest_rel());
      std::string value = m.size() == 1 ? "xe_s" : Fmt("vec%zu(xe_s)", m.size());
      Line(Fmt("%s.%s = %s;", dest.c_str(), m.c_str(),
               clamp_expr(value, alu.scalar_clamp()).c_str()));
    }
  }

  if (alu.predicated()) {
    --indent_;
    Line("}");
  }
  return true;
}

// --- Fetches -------------------------------------------------------------------

bool Translator::EmitVertexFetch(const Fetch& f) {
  if (!vs()) return Fail("vertex fetch in a pixel shader");
  ++info_.vfetch_count;
  uint32_t format = f.vf_format();
  if (format == 0 && options_.placeholder_vertex_fetch) {
    format = kVf_32_32_32_32_FLOAT;
    if (info_.approximations.empty() ||
        info_.approximations.back() != "placeholder vertex fetch (unbound blob)") {
      info_.approximations.push_back("placeholder vertex fetch (unbound blob)");
    }
  }
  if (format == 0) {
    // Left unpatched by Direct3D: the shader reads an element the current
    // vertex declaration doesn't have. Like the Xenos with an undefined
    // format, this reads no data - components come out as 0 (1 where the
    // swizzle asks for a constant 1).
    const uint32_t swizzle = f.dst_swizzle();
    std::string m, v, swz_text;
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t sw = (swizzle >> (3 * c)) & 7;
      swz_text += sw < 4 ? kComp[sw] : sw == 4 ? '0' : sw == 5 ? '1' : '_';
      if (sw == 7) continue;
      m += kComp[c];
      v += std::string(v.empty() ? "" : ", ") + (sw == 5 ? "1.0" : "0.0");
    }
    Disasm(Fmt("%s%s r%u%s.%s, element not in the declaration (reads 0)",
               f.predicated() ? (f.pred_condition() ? "(p0) " : "(!p0) ") : "",
               f.vf_mini() ? "vfetch_mini" : "vfetch_full", f.dst(), f.dst_rel() ? "[aL]" : "",
               swz_text.c_str()));
    ++info_.vfetch_count;
    const char* note = "vertex fetch of an element not in the declaration reads 0";
    if (std::find(info_.approximations.begin(), info_.approximations.end(), note) ==
        info_.approximations.end()) {
      info_.approximations.push_back(note);
    }
    if (!m.empty()) {
      if (f.predicated()) {
        Line(Fmt("if (%sxe_p0) {", f.pred_condition() ? "" : "!"));
        ++indent_;
      }
      std::string dest = Reg(f.dst(), f.dst_rel());
      Line(m.size() == 1 ? Fmt("%s.%s = %s;", dest.c_str(), m.c_str(), v.c_str())
                         : Fmt("%s.%s = vec%zu(%s);", dest.c_str(), m.c_str(), m.size(), v.c_str()));
      if (f.predicated()) {
        --indent_;
        Line("}");
      }
    }
    return true;
  }
  const char* format_name = VertexFormatName(format);
  if (!format_name) return Fail(Fmt("unknown vertex format %u", format));
  vertex_formats_.insert(format);

  uint32_t vf;
  uint32_t stride;
  if (f.vf_mini()) {
    if (!has_vfetch_full_) return Fail("vfetch_mini without a preceding vfetch_full");
    vf = last_vf_constant_;
  } else {
    vf = f.vf_constant();
    has_vfetch_full_ = true;
    last_vf_constant_ = vf;
  }
  stride = f.vf_stride();
  info_.vertex_fetch_constants[vf >> 5] |= 1u << (vf & 31);

  // Destination swizzle.
  const uint32_t swizzle = f.dst_swizzle();
  std::string swz_text;
  uint32_t write_mask = 0, needed = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t s = (swizzle >> (3 * c)) & 7;
    swz_text += s < 4 ? kComp[s] : s == 4 ? '0' : s == 5 ? '1' : '_';
    if (s != 7) write_mask |= 1u << c;
    if (s < 4) needed |= 1u << s;
  }

  std::string text = Fmt("%s r%u%s.%s, vf%u, %s, offset %d dwords", f.vf_mini() ? "vfetch_mini" : "vfetch_full",
                         f.dst(), f.dst_rel() ? "[aL]" : "", swz_text.c_str(), vf, format_name,
                         f.vf_offset());
  if (!f.vf_mini()) {
    text += Fmt(", stride %u, index r%u%s.%c%s", stride, f.src(), f.src_rel() ? "[aL]" : "",
                kComp[f.vf_src_component()], f.vf_index_rounded() ? " rounded" : "");
  }
  text += f.vf_signed() ? " signed" : " unsigned";
  text += f.vf_integer() ? " int" : " norm";
  if (f.vf_exp_adjust()) text += Fmt(" exp%+d", f.vf_exp_adjust());
  if (f.predicated()) text = (f.pred_condition() ? "(p0) " : "(!p0) ") + text;
  Disasm(text);

  if (f.predicated()) {
    Line(Fmt("if (%sxe_p0) {", f.pred_condition() ? "" : "!"));
    ++indent_;
  }
  if (!f.vf_mini()) {
    std::string index = Reg(f.src(), f.src_rel()) + "." + kComp[f.vf_src_component()];
    // Fetch constant address: physical, in dwords (the GPU ignores bits 29+).
    Line(Fmt("xe_vfa = int((xe_fetch_dword(%uu) >> 2u) & 0x07FFFFFFu) + int(floor(%s%s)) * %u;",
             vf * 2,
             index.c_str(), f.vf_index_rounded() ? " + 0.5" : "", stride));
  }
  if (write_mask) {
    // Unpack into xe_t0 (all format components; missing ones are 0).
    const int32_t off = f.vf_offset();
    const bool sgn = f.vf_signed();
    const bool norm = !f.vf_integer();
    const bool no_zero = f.vf_signed_rf_mode() == 1;
    auto word = [&](int i) { return Fmt("xe_vword(%uu, xe_vfa + %d)", vf, off + i); };
    auto packed = [&](const int* widths, const int* offsets, const int* words, int n) {
      std::string comps[4] = {"0.0", "0.0", "0.0", "0.0"};
      for (int c = 0; c < n; ++c) {
        if (!(needed & (1u << c))) continue;
        std::string w = word(words[c]);
        if (sgn) {
          std::string v = Fmt("bitfieldExtract(int(%s), %d, %d)", w.c_str(), offsets[c], widths[c]);
          comps[c] = norm ? Fmt("xe_snorm(%s, %du, %s)", v.c_str(), widths[c],
                                no_zero ? "true" : "false")
                          : Fmt("float(%s)", v.c_str());
        } else {
          std::string v = Fmt("bitfieldExtract(%s, %d, %d)", w.c_str(), offsets[c], widths[c]);
          comps[c] = norm ? Fmt("xe_unorm(%s, %du)", v.c_str(), widths[c])
                          : Fmt("float(%s)", v.c_str());
        }
      }
      return Fmt("vec4(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(),
                 comps[3].c_str());
    };
    std::string value;
    switch (format) {
      case kVf_8_8_8_8: {
        const int w[4] = {8, 8, 8, 8}, o[4] = {0, 8, 16, 24}, wd[4] = {0, 0, 0, 0};
        value = packed(w, o, wd, 4);
        break;
      }
      case kVf_2_10_10_10: {
        const int w[4] = {10, 10, 10, 2}, o[4] = {0, 10, 20, 30}, wd[4] = {0, 0, 0, 0};
        value = packed(w, o, wd, 4);
        break;
      }
      case kVf_10_11_11: {
        const int w[4] = {11, 11, 10, 0}, o[4] = {0, 11, 22, 0}, wd[4] = {0, 0, 0, 0};
        value = packed(w, o, wd, 3);
        break;
      }
      case kVf_11_11_10: {
        const int w[4] = {10, 11, 11, 0}, o[4] = {0, 10, 21, 0}, wd[4] = {0, 0, 0, 0};
        value = packed(w, o, wd, 3);
        break;
      }
      case kVf_16_16: {
        const int w[4] = {16, 16, 0, 0}, o[4] = {0, 16, 0, 0}, wd[4] = {0, 0, 0, 0};
        value = packed(w, o, wd, 2);
        break;
      }
      case kVf_16_16_16_16: {
        const int w[4] = {16, 16, 16, 16}, o[4] = {0, 16, 0, 16}, wd[4] = {0, 0, 1, 1};
        value = packed(w, o, wd, 4);
        break;
      }
      case kVf_16_16_FLOAT:
        value = (needed & 3) ? Fmt("vec4(unpackHalf2x16(%s), 0.0, 0.0)", word(0).c_str())
                             : "vec4(0.0)";
        break;
      case kVf_16_16_16_16_FLOAT:
        value = Fmt("vec4(%s, %s)",
                    (needed & 3) ? Fmt("unpackHalf2x16(%s)", word(0).c_str()).c_str() : "vec2(0.0)",
                    (needed & 12) ? Fmt("unpackHalf2x16(%s)", word(1).c_str()).c_str()
                                  : "vec2(0.0)");
        break;
      case kVf_32:
      case kVf_32_32:
      case kVf_32_32_32_32: {
        int n = format == kVf_32 ? 1 : format == kVf_32_32 ? 2 : 4;
        std::string comps[4] = {"0.0", "0.0", "0.0", "0.0"};
        for (int c = 0; c < n; ++c) {
          if (!(needed & (1u << c))) continue;
          std::string w = word(c);
          if (sgn) {
            comps[c] = norm ? (no_zero ? Fmt("(float(int(%s)) + 0.5) / 2147483647.5", w.c_str())
                                       : Fmt("max(float(int(%s)) / 2147483647.0, -1.0)", w.c_str()))
                            : Fmt("float(int(%s))", w.c_str());
          } else {
            comps[c] = norm ? Fmt("float(%s) / 4294967295.0", w.c_str())
                            : Fmt("float(%s)", w.c_str());
          }
        }
        value = Fmt("vec4(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(),
                    comps[3].c_str());
        break;
      }
      case kVf_32_FLOAT:
      case kVf_32_32_FLOAT:
      case kVf_32_32_32_FLOAT:
      case kVf_32_32_32_32_FLOAT: {
        int n = format == kVf_32_FLOAT ? 1 : format == kVf_32_32_FLOAT ? 2
              : format == kVf_32_32_32_FLOAT ? 3 : 4;
        std::string comps[4] = {"0.0", "0.0", "0.0", "0.0"};
        for (int c = 0; c < n; ++c) {
          if (needed & (1u << c)) comps[c] = Fmt("uintBitsToFloat(%s)", word(c).c_str());
        }
        value = Fmt("vec4(%s, %s, %s, %s)", comps[0].c_str(), comps[1].c_str(), comps[2].c_str(),
                    comps[3].c_str());
        break;
      }
    }
    if (f.vf_exp_adjust()) {
      value = Fmt("(%s) * %.9g", value.c_str(), double(std::ldexp(1.0f, f.vf_exp_adjust())));
    }
    Line("xe_t0 = " + value + ";");
    std::string dest = Reg(f.dst(), f.dst_rel());
    std::string m, v;
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t s = (swizzle >> (3 * c)) & 7;
      if (s == 7) continue;
      m += kComp[c];
      v += (v.empty() ? "" : ", ") + std::string(s < 4 ? "xe_t0." + std::string(1, kComp[s])
                                                       : s == 5 ? "1.0" : "0.0");
    }
    if (m.size() == 1) {
      Line(Fmt("%s.%s = %s;", dest.c_str(), m.c_str(), v.c_str()));
    } else {
      Line(Fmt("%s.%s = vec%zu(%s);", dest.c_str(), m.c_str(), m.size(), v.c_str()));
    }
  }
  if (f.predicated()) {
    --indent_;
    Line("}");
  }
  return true;
}

bool Translator::EmitTextureFetch(const Fetch& f) {
  ++info_.tfetch_count;
  const uint32_t op = f.op();
  const uint32_t dim = f.tf_dimension();
  static const char* const kDimNames[4] = {"1D", "2D", "3D", "Cube"};
  const uint32_t src_swizzle = f.tf_src_swizzle();
  auto src_components = [&](int n) {
    std::string s = Reg(f.src(), f.src_rel()) + ".";
    for (int i = 0; i < n; ++i) s += kComp[(src_swizzle >> (2 * i)) & 3];
    return s;
  };
  std::string pred_text = f.predicated() ? (f.pred_condition() ? "(p0) " : "(!p0) ") : "";
  auto open_pred = [&]() {
    if (f.predicated()) {
      Line(Fmt("if (%sxe_p0) {", f.pred_condition() ? "" : "!"));
      ++indent_;
    }
  };
  auto close_pred = [&]() {
    if (f.predicated()) {
      --indent_;
      Line("}");
    }
  };

  switch (op) {
    case kSetTextureLod:
      Disasm(pred_text + "setTexLOD " + src_components(1));
      open_pred();
      Line("xe_tf_lod = " + src_components(1) + ";");
      close_pred();
      return true;
    case kSetGradientsH:
      Disasm(pred_text + "setGradientH " + src_components(3));
      open_pred();
      Line("xe_tf_gh = " + src_components(3) + ";");
      close_pred();
      return true;
    case kSetGradientsV:
      Disasm(pred_text + "setGradientV " + src_components(3));
      open_pred();
      Line("xe_tf_gv = " + src_components(3) + ";");
      close_pred();
      return true;
    case kTextureFetch:
    case kGetBorderColorFrac:
    case kGetComputedLod:
    case kGetGradients:
    case kGetWeights:
      break;
    default:
      return Fail(Fmt("unknown fetch opcode %u", op));
  }

  // Destination.
  const uint32_t swizzle = f.dst_swizzle();
  std::string swz_text;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t s = (swizzle >> (3 * c)) & 7;
    swz_text += s < 4 ? kComp[s] : s == 4 ? '0' : s == 5 ? '1' : '_';
  }
  // 1D fetches take two coordinates too: the ps_1_x `tex` instruction
  // assembles to tfetch1D with an XY swizzle and is used on 2D textures (Amped
  // 3's D3DXAssembleShader shaders); 1D textures are 2D images one texel
  // high, so Y doesn't matter for real 1D data.
  const int coord_count = op == kGetGradients ? 2 : (dim <= 1 ? 2 : 3);
  std::string coords = src_components(coord_count);

  // Binding (textures only).
  TextureBinding* binding = nullptr;
  if (op != kGetGradients) {
    TextureBinding want;
    want.fetch_constant = f.tf_constant();
    want.dimension = dim;
    want.mag_filter = f.tf_mag() == 3 ? -1 : int(f.tf_mag());
    want.min_filter = f.tf_min() == 3 ? -1 : int(f.tf_min());
    want.mip_filter = f.tf_mip() == 3 ? -1 : int(f.tf_mip());
    want.aniso_filter = f.tf_aniso() == 7 ? -1 : int(f.tf_aniso());
    for (TextureBinding& t : info_.textures) {
      if (t.fetch_constant == want.fetch_constant && t.dimension == want.dimension &&
          t.mag_filter == want.mag_filter && t.min_filter == want.min_filter &&
          t.mip_filter == want.mip_filter && t.aniso_filter == want.aniso_filter) {
        binding = &t;
        break;
      }
    }
    if (!binding) {
      want.binding = uint32_t(info_.textures.size());
      info_.textures.push_back(want);
      binding = &info_.textures.back();
    }
  }

  static const char* const kOpNames[] = {"tfetch", "getBCF", "getCompTexLOD", "getGradients",
                                         "getWeights"};
  const char* opname = op == kTextureFetch     ? kOpNames[0]
                       : op == kGetBorderColorFrac ? kOpNames[1]
                       : op == kGetComputedLod     ? kOpNames[2]
                       : op == kGetGradients       ? kOpNames[3]
                                                   : kOpNames[4];
  std::string text = Fmt("%s%s%s r%u%s.%s, %s", pred_text.c_str(), opname,
                         op == kGetGradients ? "" : kDimNames[dim], f.dst(),
                         f.dst_rel() ? "[aL]" : "", swz_text.c_str(), coords.c_str());
  if (binding) text += Fmt(", tf%u", binding->fetch_constant);
  if (op == kTextureFetch) {
    if (f.tf_unnormalized()) text += " unnormalized";
    if (!f.tf_computed_lod()) text += " UseComputedLOD=false";
    if (f.tf_register_lod()) text += " UseRegisterLOD";
    if (f.tf_register_gradients()) text += " UseRegisterGradients";
    if (f.tf_lod_bias() != 0.0f) text += Fmt(" LODBias=%g", f.tf_lod_bias());
    for (int i = 0; i < 3; ++i) {
      if (f.tf_offset(i) != 0.0f) text += Fmt(" Offset%c=%g", "XYZ"[i], f.tf_offset(i));
    }
    if (binding && binding->mag_filter >= 0) text += Fmt(" Mag=%d", binding->mag_filter);
    if (binding && binding->min_filter >= 0) text += Fmt(" Min=%d", binding->min_filter);
    if (binding && binding->mip_filter >= 0) text += Fmt(" Mip=%d", binding->mip_filter);
    if (binding && binding->aniso_filter >= 0) text += Fmt(" Aniso=%d", binding->aniso_filter);
  }
  Disasm(text);

  open_pred();
  std::string tex = binding ? Fmt("xe_tex%u", binding->binding) : "";
  std::string value;
  if (op == kGetGradients) {
    if (vs()) return Fail("getGradients in a vertex shader");
    Line("xe_t1.xy = " + coords + ";");
    value = "vec4(dFdx(xe_t1.x), dFdy(xe_t1.x), dFdx(xe_t1.y), dFdy(xe_t1.y))";
  } else if (op == kGetBorderColorFrac) {
    info_.approximations.push_back("getBCF returns 0");
    value = "vec4(0.0)";
  } else {
    // Coordinates in xe_t1.
    if (dim <= 1) {
      Line("xe_t1 = vec4(" + coords + ", 0.0, 0.0);");
    } else {
      Line("xe_t1 = vec4(" + coords + ", 0.0);");
    }
    const bool cube = dim == 3;
    // In guest texels (rendered images are res_scale times larger).
    const std::string ts = binding ? Fmt("xe_tscale(%uu)", binding->binding) : "1.0";
    std::string size = cube ? "vec3(textureSize(" + tex + ", 0), 1.0)"
                            : (dim == 2 ? "(vec3(textureSize(" + tex + ", 0)) / vec3(" + ts + ", " +
                                              ts + ", 1.0))"
                                        : "vec3(vec2(textureSize(" + tex + ", 0)) / " + ts +
                                              ", 1.0)");
    if (f.tf_unnormalized() && !cube) {
      Line("xe_t1.xyz /= " + size + ";");
    }
    if (op == kTextureFetch) {
      float ox = f.tf_offset(0), oy = f.tf_offset(1), oz = f.tf_offset(2);
      if ((ox != 0.0f || oy != 0.0f || oz != 0.0f) && !cube) {
        Line(Fmt("xe_t1.xyz += vec3(%.9g, %.9g, %.9g) / %s;", ox, oy, dim == 2 ? oz : 0.0f,
                 size.c_str()));
      }
    }
    std::string coord;
    if (cube) {
      coord = "xe_cube_dir(xe_t1.xyz)";
    } else if (dim == 2) {
      coord = "xe_t1.xyz";
    } else {
      coord = "xe_t1.xy";
    }
    if (op == kGetComputedLod) {
      if (vs()) {
        value = "vec4(0.0)";
      } else {
        value = "vec4(textureQueryLod(" + tex + ", " + coord + ").y, 0.0, 0.0, 0.0)";
      }
    } else if (op == kGetWeights) {
      info_.approximations.push_back("getWeights ignores the LOD (mip weight = 0)");
      if (cube) {
        value = "vec4(0.0)";
      } else {
        value = "vec4(fract(xe_t1.xyz * " + size + " - 0.5), 0.0)";
      }
    } else {
      // tfetch.
      std::string lod;
      float bias = f.tf_lod_bias();
      std::string bias_expr = bias != 0.0f ? Fmt("%.9g", bias) : "";
      if (f.tf_register_lod()) bias_expr = bias_expr.empty() ? "xe_tf_lod" : bias_expr + " + xe_tf_lod";
      if (f.tf_register_gradients()) {
        std::string gh = cube ? "xe_tf_gh" : (dim == 2 ? "xe_tf_gh" : "xe_tf_gh.xy");
        std::string gv = cube ? "xe_tf_gv" : (dim == 2 ? "xe_tf_gv" : "xe_tf_gv.xy");
        value = Fmt("textureGrad(%s, %s, %s, %s)", tex.c_str(), coord.c_str(), gh.c_str(),
                    gv.c_str());
        if (!bias_expr.empty()) info_.approximations.push_back("LOD bias ignored with register gradients");
      } else if (f.tf_computed_lod() && !vs()) {
        value = bias_expr.empty() ? Fmt("texture(%s, %s)", tex.c_str(), coord.c_str())
                                  : Fmt("texture(%s, %s, %s)", tex.c_str(), coord.c_str(),
                                        bias_expr.c_str());
      } else {
        value = Fmt("textureLod(%s, %s, %s)", tex.c_str(), coord.c_str(),
                    bias_expr.empty() ? "0.0" : bias_expr.c_str());
      }
    }
  }
  Line("xe_t0 = " + value + ";");
  std::string dest = Reg(f.dst(), f.dst_rel());
  std::string m, v;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t s = (swizzle >> (3 * c)) & 7;
    if (s == 7) continue;
    m += kComp[c];
    v += (v.empty() ? "" : ", ") +
         std::string(s < 4 ? "xe_t0." + std::string(1, kComp[s]) : s == 5 ? "1.0" : "0.0");
  }
  if (!m.empty()) {
    if (m.size() == 1) {
      Line(Fmt("%s.%s = %s;", dest.c_str(), m.c_str(), v.c_str()));
    } else {
      Line(Fmt("%s.%s = vec%zu(%s);", dest.c_str(), m.c_str(), m.size(), v.c_str()));
    }
  }
  close_pred();
  return true;
}

}  // namespace

uint64_t HashMicrocode(const uint32_t* ucode, size_t dword_count) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < dword_count; ++i) {
    for (int b = 0; b < 4; ++b) {
      h ^= (ucode[i] >> (8 * b)) & 0xFF;
      h *= 0x100000001B3ull;
    }
  }
  return h;
}

bool ExtractBlobMicrocode(const uint8_t* blob, size_t size, ShaderStage* stage,
                          std::vector<uint32_t>* ucode, std::string* error) {
  auto be32 = [&](size_t offset) {
    return (uint32_t(blob[offset]) << 24) | (uint32_t(blob[offset + 1]) << 16) |
           (uint32_t(blob[offset + 2]) << 8) | uint32_t(blob[offset + 3]);
  };
  if (size < 12) {
    if (error) *error = "blob too small";
    return false;
  }
  uint32_t magic = be32(0);
  if (magic == 0x102A0E01) {
    *stage = ShaderStage::kVertex;
  } else if (magic == 0x102A0E00) {
    *stage = ShaderStage::kPixel;
  } else {
    if (error) *error = Fmt("unknown blob magic %08X", magic);
    return false;
  }
  uint32_t offset = be32(4), bytes = be32(8);
  if (offset > size || bytes > size - offset || (bytes & 3) || (offset & 3)) {
    if (error) *error = "microcode outside the blob";
    return false;
  }
  ucode->resize(bytes / 4);
  for (size_t i = 0; i < ucode->size(); ++i) (*ucode)[i] = be32(offset + i * 4);
  return true;
}

TranslateResult TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dword_count,
                                const TranslateOptions& options) {
  TranslateResult result;
  Translator translator(stage, ucode, dword_count, options);
  if (!translator.Run(result)) result.ok = false;
  return result;
}

}  // namespace amped3::native
