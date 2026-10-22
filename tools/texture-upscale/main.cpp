// amped3-texture-upscale: upscales the game's textures on the player's PC.
//
// Reads textures straight from the game's files, upscales them with
// Real-ESRGAN (realesrgan-ncnn-vulkan, any Vulkan GPU) and writes them where
// the native renderer picks them up:
//
//   <textures>/upscaled/<source>_<hash>.png
//
// <hash> is the renderer's content hash (texture_replace.h), so each PNG
// replaces exactly the texture it came from. Textures already replaced by
// hand (any <name>_<hash>.png elsewhere in <textures>) and the game's fonts
// (rebuilt by Sharp fonts) are left alone. Runs again pick up where the last
// one stopped (existing outputs are kept unless --force).
//
// Every result is checked against its original: Real-ESRGAN's Vulkan build
// returns patches of garbage on some GPUs (seen on Intel Arc: holes, black
// or missing pieces of icons). Shrunk back to the original's size, the
// result must match it; where it doesn't, a plain bicubic upscale of the
// original is blended in, and a texture that is mostly wrong is left out
// (the game's own is used). Earlier runs' outputs are checked the same way
// on every run, so running it again repairs them.
//
// Sets (--set):
//   ui     menus, HUD and loading screens: GuiPackages\*.ruirr,
//          loadingscreen.res, diesel\assets\assets.res (default)
//   world  everything else in diesel\**\*.res (experimental: characters,
//          boards, levels; textures named like normal / specular maps are
//          skipped)
//   all    both
//
// The installer runs it (optional step); by hand:
//   amped3-texture-upscale --game "<install>\game" --textures "<install>\textures"
//       --esrgan "<folder>\realesrgan-ncnn-vulkan.exe" [--model realesrgan-x4plus-anime]
//       [--set ui] [--max-size 1024] [--tile 0] [--gpu auto] [--force] [--list]
// (installer\Upscale-Textures.ps1 downloads Real-ESRGAN and runs this.)

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "gui_patch_core.h"
#include "native/texture_replace.h"
#include "native/xenos_texture.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
// stb_image (reading back) is compiled into texture_replace.cpp as static;
// this file has its own copy.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace fs = std::filesystem;
using amped3::native::TextureFetch;
using amped3::native::TextureReplacer;
using Bytes = std::vector<uint8_t>;

namespace {

struct Options {
  fs::path game, textures, esrgan, work;
  std::string model = "realesrgan-x4plus-anime";
  std::string set = "ui";
  std::string gpu;
  uint32_t max_size = 1024;
  int tile = 0;
  bool force = false, list = false, keep_work = false;
};

struct Source {
  std::string name;  // for the file name: package and path, sanitized
  TextureFetch tf;
  Bytes data;        // from the base address on
};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool ReadFile(const fs::path& p, Bytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

std::string Sanitize(std::string s) {
  for (char& c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) c = '_';
  }
  return s;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// An XPR2 resource file: every 2D texture in it.
void FromXpr(const std::string& name, const Bytes& d, std::vector<Source>* out) {
  if (d.size() < 16 || std::memcmp(d.data(), "XPR2", 4) != 0) return;
  const uint32_t header = Be32(&d[4]), count = Be32(&d[12]);
  for (uint32_t i = 0; i < count && 16 + 16 * (i + 1) <= d.size(); ++i) {
    const uint8_t* e = &d[16 + 16 * i];
    if (std::memcmp(e, "TX2D", 4) != 0) continue;
    const uint32_t off = Be32(e + 4);
    if (12 + off + 40 > d.size()) continue;
    Source s;
    for (int k = 0; k < 6; ++k) s.tf.words[k] = Be32(&d[12 + off + 16 + 4 * k]);
    const size_t data = 12 + size_t(header) + s.tf.base_address();
    if (data >= d.size()) continue;
    s.data.assign(d.begin() + data, d.end());
    s.name = count > 1 ? name + "_" + std::to_string(i) : name;
    out->push_back(std::move(s));
  }
}

// A diesel .xtx: the texture header (fetch constant at 0x10), data at 0x28.
void FromXtx(const std::string& name, const Bytes& d, std::vector<Source>* out) {
  if (d.size() <= 0x28) return;
  Source s;
  for (int k = 0; k < 6; ++k) s.tf.words[k] = Be32(&d[0x10 + 4 * k]);
  if (s.tf.type() != 2) return;
  s.data.assign(d.begin() + 0x28, d.end());
  s.name = name;
  out->push_back(std::move(s));
}

// Every texture in an archive (.ruirr: .xpr entries; .res: .xtx entries).
void FromArchive(const fs::path& path, const std::string& label, std::vector<Source>* out) {
  Bytes zip;
  if (!ReadFile(path, &zip)) return;
  std::vector<amped3::gui_patch::ZipEntry> entries;
  std::string error;
  if (!amped3::gui_patch::ReadZip(zip, &entries, &error)) {
    std::printf("  skipped %s: %s\n", path.string().c_str(), error.c_str());
    return;
  }
  for (const auto& e : entries) {
    const std::string lower = Lower(e.name);
    const std::string stem = fs::path(e.name).replace_extension().string();
    const std::string name = Sanitize(label + "_" + stem);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".xpr") == 0) {
      FromXpr(name, e.data, out);
    } else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".xtx") == 0) {
      FromXtx(name, e.data, out);
    }
  }
}

// Names the diesel tools give normal, specular and other non-colour maps:
// <name>n_<hash>, <name>s1_<hash>, ... (colour maps end in c or nothing).
bool LooksLikeDataMap(const std::string& name) {
  const std::string lower = Lower(name);
  const size_t us = lower.find_last_of('_');
  if (us == std::string::npos || us == 0) return false;
  const std::string head = lower.substr(0, us);
  for (const char* suffix : {"n", "s", "s1", "s2", "spec", "norm", "nrm", "bump", "env", "mask"}) {
    const size_t n = std::strlen(suffix);
    if (head.size() > n && head.compare(head.size() - n, n, suffix) == 0) return true;
  }
  return lower.find("normal") != std::string::npos || lower.find("lightmap") != std::string::npos;
}

bool ParseHashName(const fs::path& p, uint64_t* hash) {
  if (Lower(p.extension().string()) != ".png") return false;
  const std::string stem = p.stem().string();
  if (stem.size() < 16) return false;
  if (stem.size() > 16 && stem[stem.size() - 17] != '_') return false;
  uint64_t v = 0;
  for (char c : stem.substr(stem.size() - 16)) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = (v << 4) | uint64_t(d);
  }
  *hash = v;
  return true;
}

// Colour under transparent texels: spread from the visible ones, so the
// upscaler doesn't pull dark fringes in from what was never meant to show.
void BleedColour(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> known(size_t(w) * h);
  for (size_t i = 0; i < known.size(); ++i) known[i] = rgba[i * 4 + 3] >= 16;
  for (int pass = 0; pass < 16; ++pass) {
    std::vector<uint8_t> next = known;
    bool changed = false;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const size_t i = size_t(y) * w + x;
        if (known[i]) continue;
        int sum[3] = {0, 0, 0}, n = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = int(x) + dx, yy = int(y) + dy;
            if (xx < 0 || yy < 0 || xx >= int(w) || yy >= int(h)) continue;
            const size_t j = size_t(yy) * w + xx;
            if (!known[j]) continue;
            for (int c = 0; c < 3; ++c) sum[c] += rgba[j * 4 + c];
            ++n;
          }
        }
        if (n) {
          for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = uint8_t(sum[c] / n);
          next[i] = 1;
          changed = true;
        }
      }
    }
    known.swap(next);
    if (!changed) break;
  }
}

// Keys bicubic (a = -0.5), 4x, of RGBA8 (colour already spread under the
// transparent texels, so edges don't darken).
std::vector<uint8_t> Bicubic4x(const std::vector<uint8_t>& src, uint32_t w, uint32_t h) {
  constexpr int kS = 4;
  auto kernel = [](float x) {
    x = std::fabs(x);
    constexpr float a = -0.5f;
    if (x < 1.0f) return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
    if (x < 2.0f) return ((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a;
    return 0.0f;
  };
  // Taps and weights per output coordinate (the same each row/column).
  auto taps = [&](uint32_t n, uint32_t out_n, std::vector<int>& idx, std::vector<float>& wt) {
    idx.resize(size_t(out_n) * 4);
    wt.resize(size_t(out_n) * 4);
    for (uint32_t o = 0; o < out_n; ++o) {
      const float s = (float(o) + 0.5f) / kS - 0.5f;
      const int i0 = int(std::floor(s));
      float sum = 0;
      for (int t = 0; t < 4; ++t) {
        const int i = i0 - 1 + t;
        const float k = kernel(s - float(i));
        idx[o * 4 + t] = std::clamp(i, 0, int(n) - 1);
        wt[o * 4 + t] = k;
        sum += k;
      }
      for (int t = 0; t < 4; ++t) wt[o * 4 + t] /= sum;
    }
  };
  const uint32_t ow = w * kS, oh = h * kS;
  std::vector<int> xi, yi;
  std::vector<float> xw, yw;
  taps(w, ow, xi, xw);
  taps(h, oh, yi, yw);
  std::vector<float> rows(size_t(ow) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < ow; ++x) {
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int t = 0; t < 4; ++t) v += xw[x * 4 + t] * src[(size_t(y) * w + xi[x * 4 + t]) * 4 + c];
        rows[(size_t(y) * ow + x) * 4 + c] = v;
      }
    }
  }
  std::vector<uint8_t> out(size_t(ow) * oh * 4);
  for (uint32_t y = 0; y < oh; ++y) {
    for (uint32_t x = 0; x < ow; ++x) {
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int t = 0; t < 4; ++t) v += yw[y * 4 + t] * rows[(size_t(yi[y * 4 + t]) * ow + x) * 4 + c];
        out[(size_t(y) * ow + x) * 4 + c] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
      }
    }
  }
  return out;
}

// Checks a 4x result `up` (straight RGBA) against its original `orig` and
// mends what disagrees (see the top of the file). `bled` is the original
// with its colour spread under the transparent texels (BleedColour). Returns
// the share of the original's texels that disagreed; 0 leaves `up` as it was.
double Repair(const std::vector<uint8_t>& orig, const std::vector<uint8_t>& bled, uint32_t w,
              uint32_t h, uint8_t* up) {
  constexpr int kS = 4;
  // Calibrated on Intel Arc output: real upscaling stays under ~25 on
  // average per texel (premultiplied colour and alpha, 0..255); garbage is
  // far above it.
  constexpr float kThreshold = 32.0f;
  const uint32_t ow = w * kS;
  std::vector<uint8_t> src = bled;
  for (size_t i = 0; i < size_t(w) * h; ++i) src[i * 4 + 3] = orig[i * 4 + 3];
  const std::vector<uint8_t> smooth = Bicubic4x(src, w, h);
  // How far a 4x image, shrunk back, is from the original at a texel.
  auto error = [&](const uint8_t* img, uint32_t x, uint32_t y) {
    float sum[4] = {0, 0, 0, 0};
    for (int dy = 0; dy < kS; ++dy) {
      for (int dx = 0; dx < kS; ++dx) {
        const uint8_t* p = img + (size_t(y * kS + dy) * ow + x * kS + dx) * 4;
        const float a = p[3] / 255.0f;
        for (int c = 0; c < 3; ++c) sum[c] += p[c] * a;
        sum[3] += p[3];
      }
    }
    const uint8_t* o = &orig[(size_t(y) * w + x) * 4];
    const float oa = o[3] / 255.0f;
    float rgb = 0;
    for (int c = 0; c < 3; ++c) rgb += std::fabs(sum[c] / (kS * kS) - o[c] * oa);
    return rgb / 3.0f * 0.5f + std::fabs(sum[3] / (kS * kS) - o[3]) * 0.5f;
  };
  // Wrong where it is far off, and much further off than the plain
  // upscale (sharp edges move a little in both; a mended texel is no worse
  // than the plain upscale, so a second check leaves it alone).
  std::vector<uint8_t> bad(size_t(w) * h, 0);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const float err = error(up, x, y);
      bad[size_t(y) * w + x] = err > kThreshold && err > error(smooth.data(), x, y) + 16.0f;
    }
  }
  // One texel around each bad one too.
  std::vector<float> mask(size_t(w) * h, 0.0f);
  size_t count = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      bool any = false;
      for (int dy = -1; dy <= 1 && !any; ++dy) {
        for (int dx = -1; dx <= 1 && !any; ++dx) {
          const int xx = int(x) + dx, yy = int(y) + dy;
          if (xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h)) any = bad[size_t(yy) * w + xx];
        }
      }
      if (any) {
        mask[size_t(y) * w + x] = 1.0f;
        ++count;
      }
    }
  }
  if (!count) return 0.0;
  // The mask, bilinear at the result's size, so the mend fades in.
  for (uint32_t y = 0; y < h * kS; ++y) {
    const float sy = (float(y) + 0.5f) / kS - 0.5f;
    const int y0 = std::clamp(int(std::floor(sy)), 0, int(h) - 1), y1 = std::min(y0 + 1, int(h) - 1);
    const float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
    for (uint32_t x = 0; x < ow; ++x) {
      const float sx = (float(x) + 0.5f) / kS - 0.5f;
      const int x0 = std::clamp(int(std::floor(sx)), 0, int(w) - 1), x1 = std::min(x0 + 1, int(w) - 1);
      const float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
      const float m0 = mask[size_t(y0) * w + x0] * (1 - fx) + mask[size_t(y0) * w + x1] * fx;
      const float m1 = mask[size_t(y1) * w + x0] * (1 - fx) + mask[size_t(y1) * w + x1] * fx;
      const float m = std::min(1.0f, (m0 * (1 - fy) + m1 * fy) * 1.5f);
      if (m <= 0.0f) continue;
      uint8_t* p = up + (size_t(y) * ow + x) * 4;
      const uint8_t* s = &smooth[(size_t(y) * ow + x) * 4];
      for (int c = 0; c < 4; ++c) p[c] = uint8_t(std::lround(p[c] * (1 - m) + s[c] * m));
    }
  }
  return double(count) / double(size_t(w) * h);
}

// Repair until nothing more is wrong (mending a patch can show the garbage
// next to it). Returns the share mended in all.
double RepairAll(const std::vector<uint8_t>& orig, const std::vector<uint8_t>& bled, uint32_t w,
                 uint32_t h, uint8_t* up) {
  double total = 0.0;
  for (int pass = 0; pass < 6; ++pass) {
    const double wrong = Repair(orig, bled, w, h, up);
    total += wrong;
    if (wrong == 0.0) break;
  }
  return total;
}

// More of a texture than this wrong: the game's own is used instead.
constexpr double kMostlyWrong = 0.35;

int Run(const std::vector<std::string>& args) {
#ifdef _WIN32
  std::wstring cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += L' ';
    cmd += L'"' + fs::path(a).wstring() + L'"';
  }
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
#else
  std::string cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += ' ';
    cmd += "'" + a + "'";
  }
  return std::system(cmd.c_str());
#endif
}

void Usage() {
  std::printf(
      "amped3-texture-upscale --game <game folder> --textures <textures folder>\n"
      "    --esrgan <realesrgan-ncnn-vulkan executable> [--model realesrgan-x4plus-anime]\n"
      "    [--set ui|world|all] [--max-size 1024] [--tile 0] [--gpu auto] [--work <folder>]\n"
      "    [--force] [--keep-work] [--list]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--game") o.game = next();
    else if (a == "--textures") o.textures = next();
    else if (a == "--esrgan") o.esrgan = next();
    else if (a == "--work") o.work = next();
    else if (a == "--model") o.model = next();
    else if (a == "--set") o.set = next();
    else if (a == "--gpu") o.gpu = next();
    else if (a == "--max-size") o.max_size = uint32_t(std::stoul(next()));
    else if (a == "--tile") o.tile = std::stoi(next());
    else if (a == "--force") o.force = true;
    else if (a == "--keep-work") o.keep_work = true;
    else if (a == "--list") o.list = true;
    else {
      Usage();
      return 2;
    }
  }
  if (o.game.empty() || o.textures.empty() || (o.esrgan.empty() && !o.list)) {
    Usage();
    return 2;
  }
  const fs::path out_dir = o.textures / TextureReplacer::kUpscaledFolder;
  std::error_code ec;
  // Scratch space outside the textures folder (its PNGs are named by hash
  // too, and the renderer would take them for replacements).
  if (o.work.empty()) o.work = fs::temp_directory_path(ec) / "amped3-texture-upscale";

  // What's there already: hand-made replacements (left alone) and earlier
  // runs' output (kept).
  std::unordered_set<uint64_t> manual;
  std::unordered_map<uint64_t, fs::path> done;
  if (fs::is_directory(o.textures, ec)) {
    for (auto it = fs::recursive_directory_iterator(o.textures, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_directory(ec)) {
        const auto name = it->path().filename();
        if (it.depth() == 0 && name == TextureReplacer::kUpscaledFolder) {
          it.disable_recursion_pending();
        }
        continue;
      }
      uint64_t h;
      if (ParseHashName(it->path(), &h)) manual.insert(h);
    }
  }
  if (!o.force && fs::is_directory(out_dir, ec)) {
    for (const auto& e : fs::directory_iterator(out_dir, ec)) {
      uint64_t h;
      if (ParseHashName(e.path(), &h)) done[h] = e.path();
    }
  }

  // The textures.
  std::vector<Source> sources;
  const bool ui = o.set == "ui" || o.set == "all";
  const bool world = o.set == "world" || o.set == "all";
  if (ui) {
    for (const auto& e : fs::directory_iterator(o.game / "GuiPackages", ec)) {
      if (Lower(e.path().extension().string()) == ".ruirr") {
        FromArchive(e.path(), e.path().stem().string(), &sources);
      }
    }
    FromArchive(o.game / "loadingscreen.res", "loadingscreen", &sources);
    FromArchive(o.game / "diesel" / "assets" / "assets.res", "assets", &sources);
  }
  if (world) {
    const fs::path diesel = o.game / "diesel";
    for (auto it = fs::recursive_directory_iterator(diesel, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".res") continue;
      if (it->path() == diesel / "assets" / "assets.res") continue;
      const std::string rel = fs::relative(it->path(), diesel, ec).replace_extension().string();
      std::vector<Source> found;
      FromArchive(it->path(), rel, &found);
      for (auto& s : found) {
        if (!LooksLikeDataMap(s.name)) sources.push_back(std::move(s));
      }
    }
  }

  // Which to do.
  struct Job {
    std::string name;
    uint64_t hash;
    uint32_t w, h;
    std::vector<uint8_t> rgba;
    bool alpha;
  };
  std::vector<Job> jobs;
  std::vector<std::pair<Job, fs::path>> checks;  // earlier runs' results, to check
  std::set<uint64_t> seen;
  size_t skipped_manual = 0, skipped_font = 0, skipped_done = 0, skipped_other = 0;
  for (auto& s : sources) {
    const TextureFetch& tf = s.tf;
    if (tf.dimension() != 1 || !amped3::native::BcFormatFor(tf.format())) {
      ++skipped_other;
      continue;
    }
    const uint64_t hash = TextureReplacer::ContentHash(tf, s.data.data(), s.data.size());
    if (!hash || !seen.insert(hash).second) continue;
    if (manual.count(hash)) { ++skipped_manual; continue; }
    if (TextureReplacer::IsFont(hash)) { ++skipped_font; continue; }
    auto done_it = done.find(hash);
    if (done_it != done.end() && o.list) { ++skipped_done; continue; }
    const uint32_t w = tf.width(), h = tf.height();
    // RGB in natural order, as the renderer expects a pack PNG: only the
    // identity colour swizzle (alpha may read as one).
    const uint32_t sw = tf.swizzle();
    if ((sw & 0x1FF) != 0x088 || w < 16 || h < 16 || std::max(w, h) > o.max_size) {
      ++skipped_other;
      continue;
    }
    Job j{s.name, hash, w, h, {}, false};
    if (o.list) {
      std::printf("%016llX %4ux%-4u %s %s\n", static_cast<unsigned long long>(hash), w, h,
                  amped3::native::TextureFormatName(tf.format()), s.name.c_str());
      continue;
    }
    auto decoded = amped3::native::DecodeTexture(tf, s.data.data(), s.data.size());
    if (!decoded.note.empty() || decoded.rgba.size() != size_t(w) * h * 4) {
      ++skipped_other;
      continue;
    }
    j.rgba = std::move(decoded.rgba);
    for (size_t i = 3; i < j.rgba.size(); i += 4) {
      if (j.rgba[i] < 250) {
        j.alpha = true;
        break;
      }
    }
    if (done_it != done.end()) {
      ++skipped_done;
      checks.emplace_back(std::move(j), done_it->second);
      continue;
    }
    jobs.push_back(std::move(j));
  }
  std::printf("%zu textures found: %zu to upscale; left alone: %zu replaced by hand, %zu fonts, "
              "%zu done before, %zu other (format, size, layout)\n",
              seen.size(), o.list ? 0 : jobs.size(), skipped_manual, skipped_font, skipped_done,
              skipped_other);
  if (o.list) return 0;

  // Earlier runs' results: mended or removed if the upscaler garbled them.
  if (!checks.empty()) {
    size_t mended = 0, removed = 0;
    for (auto& [j, path] : checks) {
      int w = 0, h = 0, n = 0;
      stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
      if (!px) continue;
      if (w == int(j.w * 4) && h == int(j.h * 4)) {
        std::vector<uint8_t> bled = j.rgba;
        if (j.alpha) BleedColour(bled, j.w, j.h);
        const double wrong = RepairAll(j.rgba, bled, j.w, j.h, px);
        if (wrong > kMostlyWrong) {
          stbi_image_free(px);
          px = nullptr;
          fs::remove(path, ec);
          ++removed;
        } else if (wrong > 0.0 && stbi_write_png(path.string().c_str(), w, h, 4, px, w * 4)) {
          ++mended;
        }
      }
      if (px) stbi_image_free(px);
    }
    std::printf("Checked %zu earlier results: %zu mended, %zu removed (mostly garbled; the "
                "game's own is used)\n",
                checks.size(), mended, removed);
  }
  if (jobs.empty()) return 0;

  // Inputs for the upscaler: colour (alpha spread out of view) and, for
  // textures with transparency, the alpha as a grey image of its own.
  const fs::path in_dir = o.work / "in", up_dir = o.work / "out";
  fs::remove_all(o.work, ec);
  fs::create_directories(in_dir, ec);
  fs::create_directories(up_dir, ec);
  fs::create_directories(out_dir, ec);
  for (auto& j : jobs) {
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(j.hash));
    std::vector<uint8_t> rgb = j.rgba;
    if (j.alpha) BleedColour(rgb, j.w, j.h);
    for (size_t i = 3; i < rgb.size(); i += 4) rgb[i] = 255;
    stbi_write_png((in_dir / (std::string(hex) + ".png")).string().c_str(), int(j.w), int(j.h), 4,
                   rgb.data(), int(j.w) * 4);
    if (j.alpha) {
      std::vector<uint8_t> a(size_t(j.w) * j.h * 3);
      for (size_t i = 0; i < size_t(j.w) * j.h; ++i) a[i * 3] = a[i * 3 + 1] = a[i * 3 + 2] = j.rgba[i * 4 + 3];
      stbi_write_png((in_dir / (std::string(hex) + "_alpha.png")).string().c_str(), int(j.w),
                     int(j.h), 3, a.data(), int(j.w) * 3);
    }
  }

  // Real-ESRGAN, once over the folder.
  std::vector<std::string> cmd = {o.esrgan.string(), "-i", in_dir.string(), "-o", up_dir.string(),
                                  "-n", o.model, "-s", "4", "-f", "png"};
  const fs::path models = o.esrgan.parent_path() / "models";
  if (fs::is_directory(models, ec)) {
    cmd.push_back("-m");
    cmd.push_back(models.string());
  }
  if (o.tile > 0) {
    cmd.push_back("-t");
    cmd.push_back(std::to_string(o.tile));
  }
  if (!o.gpu.empty() && o.gpu != "auto") {
    cmd.push_back("-g");
    cmd.push_back(o.gpu);
  }
  std::printf("Upscaling %zu textures with %s (%s)...\n", jobs.size(), o.model.c_str(),
              o.esrgan.filename().string().c_str());
  std::fflush(stdout);
  const auto t0 = std::chrono::steady_clock::now();
  const int rc = Run(cmd);
  if (rc != 0) {
    std::printf("realesrgan-ncnn-vulkan failed (exit %d)\n", rc);
    return 1;
  }

  // Back together: colour, alpha, into upscaled\<name>_<hash>.png.
  size_t written = 0, failed = 0, mended = 0, dropped = 0;
  std::ofstream manifest(out_dir / "manifest.csv", std::ios::app);
  for (const auto& j : jobs) {
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(j.hash));
    int w = 0, h = 0, n = 0;
    stbi_uc* rgb = stbi_load((up_dir / (std::string(hex) + ".png")).string().c_str(), &w, &h, &n, 4);
    if (!rgb || w != int(j.w * 4) || h != int(j.h * 4)) {
      if (rgb) stbi_image_free(rgb);
      ++failed;
      continue;
    }
    if (j.alpha) {
      int aw = 0, ah = 0, an = 0;
      stbi_uc* a = stbi_load((up_dir / (std::string(hex) + "_alpha.png")).string().c_str(), &aw,
                             &ah, &an, 1);
      if (!a || aw != w || ah != h) {
        if (a) stbi_image_free(a);
        stbi_image_free(rgb);
        ++failed;
        continue;
      }
      for (size_t i = 0; i < size_t(w) * h; ++i) rgb[i * 4 + 3] = a[i];
      stbi_image_free(a);
    }
    {
      std::vector<uint8_t> bled = j.rgba;
      if (j.alpha) BleedColour(bled, j.w, j.h);
      const double wrong = RepairAll(j.rgba, bled, j.w, j.h, rgb);
      if (wrong > kMostlyWrong) {
        stbi_image_free(rgb);
        ++dropped;
        continue;
      }
      if (wrong > 0.0) ++mended;
    }
    const std::string file = j.name + "_" + hex + ".png";
    if (stbi_write_png((out_dir / file).string().c_str(), w, h, 4, rgb, w * 4)) {
      ++written;
      manifest << hex << "," << j.name << "," << j.w << "x" << j.h << "," << o.model << "\n";
    } else {
      ++failed;
    }
    stbi_image_free(rgb);
  }
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%zu textures upscaled into %s in %.0f s%s\n", written, out_dir.string().c_str(), s,
              failed ? (" (" + std::to_string(failed) + " failed)").c_str() : "");
  if (mended || dropped) {
    std::printf("The upscaler garbled parts of %zu: %zu mended, %zu left out (the game's own is "
                "used)\n",
                mended + dropped, mended, dropped);
  }
  if (!o.keep_work) fs::remove_all(o.work, ec);
  return failed && !written ? 1 : 0;
}
