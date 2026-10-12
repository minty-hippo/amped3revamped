// Offline shader translation for the native renderer (step 3).
//
//   amped3-shader-translate <shader .bin | folder> ... -o <out folder>
//                           [--glslang <glslang or glslangValidator>]
//                           [--spirv-val <spirv-val>] [--blobs]
//
// Inputs are the files written by the capture_shaders setting (logs/shaders/):
// ucode_vs_*.bin / ucode_ps_*.bin hold the microcode as the GPU ran it (vertex
// fetches patched for the declaration), vs_*.bin / ps_*.bin the blobs given to
// CreateVertexShader / CreatePixelShader. Folders contribute their ucode_*.bin
// files, plus the blobs with --blobs. Blob vertex shaders contain placeholder
// vertex fetches, which --blobs translates as 4 x float32 to check everything
// else. For each one this writes <stem>.vert / <stem>.frag
// (GLSL), <stem>.txt (microcode listing and interface summary) and, with
// --glslang, <stem>.spv compiled for Vulkan 1.1, then checked with spirv-val.
// summary.txt lists every shader with its result. The exit code is non-zero
// if any shader fails to translate, compile or validate.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/native/xenos_shader.h"

namespace fs = std::filesystem;
using amped3::native::ShaderInfo;
using amped3::native::ShaderStage;
using amped3::native::TranslateResult;

namespace {

std::string Quote(const fs::path& p) {
  std::string s = p.string();
#ifdef _WIN32
  return "\"" + s + "\"";
#else
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
#endif
}

int Run(const std::string& command, const fs::path& log) {
  std::string full = command + " > " + Quote(log) + " 2>&1";
#ifdef _WIN32
  full = "\"" + full + "\"";  // cmd /c strips the outer quotes
#endif
  int rc = std::system(full.c_str());
  return rc;
}

std::string ReadText(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string Bitmap(const uint32_t* words, uint32_t count, const char* prefix) {
  std::string s;
  for (uint32_t i = 0; i < count; ++i) {
    if (words[i >> 5] & (1u << (i & 31))) {
      if (!s.empty()) s += ' ';
      s += prefix + std::to_string(i);
    }
  }
  return s.empty() ? "-" : s;
}

std::string Describe(const ShaderInfo& info) {
  std::ostringstream o;
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llX", (unsigned long long)info.ucode_hash);
  o << "stage: " << (info.stage == ShaderStage::kVertex ? "vertex" : "pixel") << "\n";
  o << "microcode: " << info.ucode_dwords << " dwords, hash " << hash << "\n";
  o << "instructions: " << info.cf_count << " control flow, " << info.alu_count << " ALU, "
    << info.vfetch_count << " vertex fetch, " << info.tfetch_count << " texture fetch\n";
  o << "registers: " << info.register_count << (info.dynamic_registers ? " (aL-relative)" : "")
    << "\n";
  o << "float constants: " << Bitmap(info.float_constants, 256, "c")
    << (info.dynamic_constants ? " + relative" : "") << "\n";
  o << "bool constants: " << (info.uses_bool_constants ? "yes" : "no")
    << ", loop constants: " << (info.uses_loop_constants ? "yes" : "no")
    << ", jumps/loops: " << (info.has_jumps ? "yes" : "no") << "\n";
  if (info.stage == ShaderStage::kVertex) {
    o << "vertex fetch constants: " << Bitmap(info.vertex_fetch_constants, 96, "vf") << "\n";
    o << "vertex formats:";
    for (uint32_t f : info.vertex_formats) o << " " << f;
    o << "\n";
    o << "interpolators written: " << Bitmap(&info.interpolators_written, 16, "o")
      << (info.writes_point_size ? ", point size" : "") << "\n";
  } else {
    o << "colors written: " << Bitmap(&info.colors_written, 4, "oC")
      << (info.writes_depth ? ", depth" : "") << (info.kills ? ", kills pixels" : "") << "\n";
  }
  for (const auto& t : info.textures) {
    static const char* const kDims[4] = {"1D", "2D", "3D", "cube"};
    o << "texture binding " << t.binding << ": tf" << t.fetch_constant << " " << kDims[t.dimension & 3];
    if (t.mag_filter >= 0) o << " mag=" << t.mag_filter;
    if (t.min_filter >= 0) o << " min=" << t.min_filter;
    if (t.mip_filter >= 0) o << " mip=" << t.mip_filter;
    if (t.aniso_filter >= 0) o << " aniso=" << t.aniso_filter;
    o << "\n";
  }
  for (const auto& a : info.approximations) o << "approximation: " << a << "\n";
  return o.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  fs::path out_dir;
  std::string glslang, spirv_val;
  amped3::native::TranslateOptions options;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (a == "--glslang" && i + 1 < argc) {
      glslang = argv[++i];
    } else if (a == "--spirv-val" && i + 1 < argc) {
      spirv_val = argv[++i];
    } else if (a == "--blobs" || a == "--placeholder-vfetch") {
      options.placeholder_vertex_fetch = true;
    } else if (a == "-h" || a == "--help") {
      std::cout << "usage: amped3-shader-translate <shader .bin | folder> ... -o <out folder> "
                   "[--glslang <exe>] [--spirv-val <exe>] [--blobs]\n";
      return 0;
    } else {
      inputs.push_back(a);
    }
  }
  if (inputs.empty() || out_dir.empty()) {
    std::cerr << "usage: amped3-shader-translate <shader .bin | folder> ... -o <out folder> "
                 "[--glslang <exe>] [--spirv-val <exe>] [--blobs]\n";
    return 2;
  }
  std::vector<fs::path> files;
  for (const auto& in : inputs) {
    if (fs::is_directory(in)) {
      for (const auto& e : fs::directory_iterator(in)) {
        std::string name = e.path().filename().string();
        const bool blob = name.rfind("vs_", 0) == 0 || name.rfind("ps_", 0) == 0;
        const bool ucode = name.rfind("ucode_vs_", 0) == 0 || name.rfind("ucode_ps_", 0) == 0;
        if (e.path().extension() == ".bin" &&
            (ucode || (blob && options.placeholder_vertex_fetch))) {
          files.push_back(e.path());
        }
      }
    } else {
      files.push_back(in);
    }
  }
  std::sort(files.begin(), files.end());
  fs::create_directories(out_dir);

  int translated = 0, compiled = 0, validated = 0, failed = 0;
  std::map<std::string, int> approximations;
  std::ofstream summary(out_dir / "summary.txt");
  for (const auto& file : files) {
    std::string stem = file.stem().string();
    std::string blob = ReadText(file);
    ShaderStage stage;
    std::vector<uint32_t> ucode;
    std::string error;
    std::string status;
    bool extracted;
    if (stem.rfind("ucode_", 0) == 0) {
      // Raw big-endian microcode.
      stage = stem.rfind("ucode_vs_", 0) == 0 ? ShaderStage::kVertex : ShaderStage::kPixel;
      ucode.resize(blob.size() / 4);
      const auto* b = reinterpret_cast<const uint8_t*>(blob.data());
      for (size_t i = 0; i < ucode.size(); ++i) {
        ucode[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) |
                   (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
      }
      extracted = !ucode.empty();
      if (!extracted) error = "empty file";
    } else {
      extracted = amped3::native::ExtractBlobMicrocode(
          reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), &stage, &ucode, &error);
    }
    if (!extracted) {
      status = "BLOB ERROR: " + error;
      ++failed;
    } else {
      TranslateResult r = amped3::native::TranslateShader(stage, ucode.data(), ucode.size(), options);
      if (!r.ok) {
        status = "TRANSLATE ERROR: " + r.error;
        ++failed;
      } else {
        ++translated;
        for (const auto& a : r.info.approximations) ++approximations[a];
        fs::path glsl = out_dir / (stem + (stage == ShaderStage::kVertex ? ".vert" : ".frag"));
        std::ofstream(glsl, std::ios::binary) << r.glsl;
        std::ofstream(out_dir / (stem + ".txt"), std::ios::binary)
            << Describe(r.info) << "\n" << r.disasm;
        status = "translated";
        if (!glslang.empty()) {
          fs::path spv = out_dir / (stem + ".spv");
          fs::path log = out_dir / (stem + ".log");
          int rc = Run(Quote(glslang) + " -V --target-env vulkan1.1 -o " + Quote(spv) + " " +
                           Quote(glsl),
                       log);
          if (rc != 0) {
            status = "GLSLANG ERROR (see " + log.filename().string() + ")";
            ++failed;
          } else {
            ++compiled;
            status = "compiled";
            if (!spirv_val.empty()) {
              rc = Run(Quote(spirv_val) + " --target-env vulkan1.1 " + Quote(spv), log);
              if (rc != 0) {
                status = "SPIRV-VAL ERROR (see " + log.filename().string() + ")";
                ++failed;
              } else {
                ++validated;
                status = "valid";
                std::error_code ec;
                fs::remove(log, ec);
              }
            } else {
              std::error_code ec;
              fs::remove(log, ec);
            }
          }
        }
      }
    }
    summary << stem << ": " << status << "\n";
    if (status != "valid" && status != "compiled" && status != "translated") {
      std::cerr << stem << ": " << status << "\n";
    }
  }
  std::ostringstream totals;
  totals << files.size() << " shaders: " << translated << " translated";
  if (!glslang.empty()) totals << ", " << compiled << " compiled";
  if (!spirv_val.empty()) totals << ", " << validated << " passed spirv-val";
  totals << ", " << failed << " failed\n";
  for (const auto& [text, count] : approximations) {
    totals << "approximation in " << count << " shader(s): " << text << "\n";
  }
  summary << "\n" << totals.str();
  std::cout << totals.str();
  return failed ? 1 : 0;
}
