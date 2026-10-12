// amped3 - texture replacements for the native renderer (see texture_replace.h).

#include "texture_replace.h"

#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace amped3::native {
namespace {

// The game's fonts (GuiPackages Base.ruirr / Game.ruirr Fonts\*.xpr), by
// ContentHash: rebuilt at 4x with hd_fonts.
struct KnownFont {
  uint64_t hash;
  const char* name;
};
constexpr KnownFont kFonts[] = {
    {0x55C4E466D81E083Aull, "BronzeBigFont_30"},     // titles, menu items (512x512)
    {0x7DE883665797A1E4ull, "NewAgency_18"},         // small text, button legends (256x512)
    {0xCD6AA4AF88CEE017ull, "XMRadio_supernatural_18"},  // the radio (256x256)
    {0xE83086C4D1D718C5ull, "CrashNumbers"},         // crash scores (256x256)
    {0x283E0BB38D47E7E9ull, "NewFunkNumbers"},       // trick scores (256x256)
};
constexpr uint32_t kFontScale = 4;
// Bumped when UpscaleGlyphAtlas changes, so cached fonts are rebuilt.
constexpr uint32_t kFontVersion = 3;

// The cache file: "A3FN", version, width, height (little-endian 32-bit), then
// the RGBA texels, deflated.
std::string FontCachePath(const std::string& folder, uint64_t hash) {
  if (folder.empty()) return {};
  char name[64];
  std::snprintf(name, sizeof(name), "native_font_%016llX.bin", static_cast<unsigned long long>(hash));
  return (std::filesystem::path(folder) / name).string();
}

FILE* OpenFile(const std::string& path, const char* mode) {
#ifdef _WIN32
  FILE* f = nullptr;
  std::wstring wmode(mode, mode + std::strlen(mode));
  _wfopen_s(&f, std::filesystem::path(path).wstring().c_str(), wmode.c_str());
  return f;
#else
  return std::fopen(path.c_str(), mode);
#endif
}

bool LoadFontCache(const std::string& path, uint32_t w, uint32_t h, std::vector<uint8_t>* rgba) {
  if (path.empty()) return false;
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> data;
  uint8_t buf[65536];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + got);
  std::fclose(f);
  if (data.size() < 16) return false;
  uint32_t head[4];
  std::memcpy(head, data.data(), 16);
  if (head[0] != 0x4E463341u || head[1] != kFontVersion || head[2] != w || head[3] != h) return false;
  size_t out_len = 0;
  void* out = tinfl_decompress_mem_to_heap(data.data() + 16, data.size() - 16, &out_len, 0);
  if (!out) return false;
  const bool ok = out_len == size_t(w) * h * 4;
  if (ok) rgba->assign(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + out_len);
  mz_free(out);
  return ok;
}

void SaveFontCache(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  if (path.empty()) return;
  size_t len = 0;
  void* packed = tdefl_compress_mem_to_heap(rgba.data(), rgba.size(), &len, 16);
  if (!packed) return;
  const std::string tmp = path + ".tmp";
  if (FILE* f = OpenFile(tmp, "wb")) {
    const uint32_t head[4] = {0x4E463341u, kFontVersion, w, h};  // "A3FN"
    const bool ok = std::fwrite(head, 4, 4, f) == 4 && std::fwrite(packed, 1, len, f) == len;
    if (std::fclose(f) == 0 && ok) {
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      if (ec) std::filesystem::remove(tmp, ec);
    } else {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
    }
  }
  mz_free(packed);
}

const KnownFont* FontFor(uint64_t hash) {
  for (const auto& f : kFonts) {
    if (f.hash == hash) return &f;
  }
  return nullptr;
}

uint32_t BlockBytes(uint32_t bc) { return bc == 1 || bc == 4 ? 8 : 16; }

// "<anything>_<16 hex digits>.png" or "<16 hex digits>.png" -> the hash.
bool ParsePackName(const std::filesystem::path& path, uint64_t* hash) {
  std::string ext = path.extension().string();
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (ext != ".png") return false;
  const std::string stem = path.stem().string();
  if (stem.size() < 16) return false;
  const std::string hex = stem.substr(stem.size() - 16);
  if (stem.size() > 16 && stem[stem.size() - 17] != '_') return false;
  uint64_t v = 0;
  for (char c : hex) {
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

// --- glyph atlas rebuild ----------------------------------------------------

// Separable Gaussian blur (edges clamped).
std::vector<float> Blur(const std::vector<float>& src, int w, int h, float sigma) {
  const int r = std::max(1, int(std::ceil(sigma * 3.0f)));
  std::vector<float> k(2 * r + 1);
  float sum = 0.0f;
  for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
  for (float& v : k) v /= sum;
  std::vector<float> tmp(src.size()), out(src.size());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc = 0.0f;
      for (int i = -r; i <= r; ++i) acc += k[i + r] * src[size_t(y) * w + std::clamp(x + i, 0, w - 1)];
      tmp[size_t(y) * w + x] = acc;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc = 0.0f;
      for (int i = -r; i <= r; ++i) acc += k[i + r] * tmp[size_t(std::clamp(y + i, 0, h - 1)) * w + x];
      out[size_t(y) * w + x] = acc;
    }
  }
  return out;
}

// Catmull-Rom weights for a fractional position.
inline void CubicWeights(float t, float wt[4]) {
  const float t2 = t * t, t3 = t2 * t;
  wt[0] = 0.5f * (-t3 + 2.0f * t2 - t);
  wt[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
  wt[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
  wt[3] = 0.5f * (t3 - t2);
}

inline float Luma(const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
inline float Saturation(const float* c) {
  return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
}


// --- outlines: the glyphs as vector shapes -----------------------------------

struct Pt {
  float x, y;
};
using Loop = std::vector<Pt>;

// Closed outlines where `f` (fw x fh samples, sample i at x = i) crosses
// `level`, by marching squares (outside the field counts as below).
std::vector<Loop> TraceContours(const std::vector<float>& f, int fw, int fh, float level) {
  // Samples padded by one on each side (value 0) so every outline closes.
  const int pw = fw + 2, ph = fh + 2;
  auto at = [&](int x, int y) -> float {
    x -= 1;
    y -= 1;
    if (x < 0 || y < 0 || x >= fw || y >= fh) return 0.0f;
    return f[size_t(y) * fw + x];
  };
  // Crossing points live on cell edges: horizontal edge (x,y)-(x+1,y) is id
  // 2*(y*pw+x), vertical (x,y)-(x,y+1) is 2*(y*pw+x)+1.
  std::unordered_map<int64_t, Pt> points;
  std::unordered_map<int64_t, std::array<int64_t, 2>> links;
  auto edge_point = [&](int64_t id) -> Pt {
    auto it = points.find(id);
    if (it != points.end()) return it->second;
    const int64_t cell = id >> 1;
    const int x = int(cell % pw), y = int(cell / pw);
    const bool vertical = id & 1;
    const float a = at(x, y), b = vertical ? at(x, y + 1) : at(x + 1, y);
    const float t = std::clamp((level - a) / (b - a), 0.0f, 1.0f);
    Pt p = vertical ? Pt{float(x - 1), float(y - 1) + t} : Pt{float(x - 1) + t, float(y - 1)};
    points.emplace(id, p);
    return p;
  };
  auto link = [&](int64_t a, int64_t b) {
    edge_point(a);
    edge_point(b);
    auto add = [&](int64_t from, int64_t to) {
      auto it = links.find(from);
      if (it == links.end()) links.emplace(from, std::array<int64_t, 2>{to, -1});
      else it->second[1] = to;
    };
    add(a, b);
    add(b, a);
  };
  for (int y = 0; y + 1 < ph; ++y) {
    for (int x = 0; x + 1 < pw; ++x) {
      const float v0 = at(x, y), v1 = at(x + 1, y), v2 = at(x + 1, y + 1), v3 = at(x, y + 1);
      const int c = (v0 >= level ? 1 : 0) | (v1 >= level ? 2 : 0) | (v2 >= level ? 4 : 0) |
                    (v3 >= level ? 8 : 0);
      if (c == 0 || c == 15) continue;
      const int64_t top = 2 * (int64_t(y) * pw + x), bottom = 2 * (int64_t(y + 1) * pw + x);
      const int64_t left = 2 * (int64_t(y) * pw + x) + 1, right = 2 * (int64_t(y) * pw + x + 1) + 1;
      switch (c) {
        case 1: case 14: link(left, top); break;
        case 2: case 13: link(top, right); break;
        case 3: case 12: link(left, right); break;
        case 4: case 11: link(right, bottom); break;
        case 6: case 9: link(top, bottom); break;
        case 7: case 8: link(left, bottom); break;
        case 5: case 10: {
          // Saddle: the centre decides which corners join.
          const bool centre = (v0 + v1 + v2 + v3) * 0.25f >= level;
          if ((c == 5) == centre) {
            link(left, bottom);
            link(top, right);
          } else {
            link(left, top);
            link(right, bottom);
          }
          break;
        }
      }
    }
  }
  std::vector<Loop> loops;
  std::unordered_map<int64_t, bool> seen;
  for (const auto& [start, ends] : links) {
    if (seen.count(start)) continue;
    Loop loop;
    int64_t prev = -1, cur = start;
    while (true) {
      seen[cur] = true;
      loop.push_back(points[cur]);
      const auto& n = links[cur];
      int64_t next = n[0] != prev ? n[0] : n[1];
      if (next < 0 || next == start || seen.count(next)) break;
      prev = cur;
      cur = next;
    }
    if (loop.size() >= 3) loops.push_back(std::move(loop));
  }
  return loops;
}

float LoopArea(const Loop& l) {
  double a = 0;
  for (size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) a += double(l[j].x) * l[i].y - double(l[i].x) * l[j].y;
  return float(std::fabs(a) * 0.5);
}

// Douglas-Peucker on [first, last] of `l` (indices, last may wrap).
void Simplify(const Loop& l, size_t first, size_t last, float eps, std::vector<char>& keep) {
  const size_t n = l.size();
  if ((last + n - first) % n < 2) return;
  const Pt a = l[first], b = l[last % n];
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
  float best = -1.0f;
  size_t best_i = first;
  for (size_t i = (first + 1) % n; i != last % n; i = (i + 1) % n) {
    const float d = std::fabs((l[i].x - a.x) * dy - (l[i].y - a.y) * dx) / len;
    if (d > best) best = d, best_i = i;
  }
  if (best > eps) {
    keep[best_i] = 1;
    Simplify(l, first, best_i, eps, keep);
    Simplify(l, best_i, last, eps, keep);
  }
}

// The outline as a polygon with few corners: straight runs become straight
// edges, sharp turns stay sharp corners, gentle ones become curves.
Loop Vectorize(const Loop& l, float eps, float corner_cos) {
  const size_t n = l.size();
  // Split at the point farthest from the first, simplify both halves.
  size_t far = 0;
  float fd = -1;
  for (size_t i = 0; i < n; ++i) {
    const float d = (l[i].x - l[0].x) * (l[i].x - l[0].x) + (l[i].y - l[0].y) * (l[i].y - l[0].y);
    if (d > fd) fd = d, far = i;
  }
  std::vector<char> keep(n, 0);
  keep[0] = keep[far] = 1;
  Simplify(l, 0, far, eps, keep);
  Simplify(l, far, n, eps, keep);
  Loop v;
  for (size_t i = 0; i < n; ++i) {
    if (keep[i]) v.push_back(l[i]);
  }
  if (v.size() < 3) return v;
  Loop out;
  const size_t m = v.size();
  for (size_t i = 0; i < m; ++i) {
    const Pt p = v[(i + m - 1) % m], q = v[i], r = v[(i + 1) % m];
    const Pt m0{(p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f}, m1{(q.x + r.x) * 0.5f, (q.y + r.y) * 0.5f};
    const float d1x = q.x - p.x, d1y = q.y - p.y, d2x = r.x - q.x, d2y = r.y - q.y;
    const float c = (d1x * d2x + d1y * d2y) /
                    std::max(std::sqrt((d1x * d1x + d1y * d1y) * (d2x * d2x + d2y * d2y)), 1e-9f);
    if (c < corner_cos) {
      out.push_back(m0);
      out.push_back(q);  // a corner
    } else {
      // A quadratic curve from one edge's middle to the next's, through q's pull.
      constexpr int kSteps = 8;
      for (int s = 0; s < kSteps; ++s) {
        const float t = float(s) / kSteps, u = 1.0f - t;
        out.push_back({u * u * m0.x + 2 * u * t * q.x + t * t * m1.x,
                       u * u * m0.y + 2 * u * t * q.y + t * t * m1.y});
      }
    }
  }
  return out;
}

// Coverage (0..1) of the shapes (even-odd), w x h pixels; loop points in
// pixel units with pixel (x, y) covering [x, x+1) x [y, y+1). 8 sub-rows per
// pixel, exact coverage along each.
std::vector<float> FillLoops(const std::vector<Loop>& loops, int w, int h) {
  struct Edge {
    float x0, y0, x1, y1;
  };
  std::vector<Edge> edges;
  for (const Loop& l : loops) {
    for (size_t i = 0; i < l.size(); ++i) {
      const Pt a = l[i], b = l[(i + 1) % l.size()];
      if (a.y == b.y) continue;
      edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y} : Edge{b.x, b.y, a.x, a.y});
    }
  }
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
  std::vector<float> cov(size_t(w) * h, 0.0f);
  constexpr int kSub = 8;
  std::vector<size_t> active;
  std::vector<float> xs;
  std::vector<float> row(size_t(w) + 2);
  size_t next = 0;
  for (int y = 0; y < h; ++y) {
    std::fill(row.begin(), row.end(), 0.0f);
    for (int s = 0; s < kSub; ++s) {
      const float sy = y + (s + 0.5f) / kSub;
      while (next < edges.size() && edges[next].y0 <= sy) active.push_back(next++);
      xs.clear();
      for (size_t k = 0; k < active.size();) {
        const Edge& e = edges[active[k]];
        if (e.y1 <= sy) {
          active[k] = active.back();
          active.pop_back();
          continue;
        }
        if (e.y0 <= sy) xs.push_back(e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0));
        ++k;
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        const float a = std::clamp(xs[k], 0.0f, float(w)), b = std::clamp(xs[k + 1], 0.0f, float(w));
        if (b <= a) continue;
        const int ia = int(a), ib = int(b);
        if (ia == ib) {
          row[ia] += b - a;
          continue;
        }
        row[ia] += float(ia + 1) - a;
        for (int i = ia + 1; i < ib; ++i) row[i] += 1.0f;
        if (ib < w) row[ib] += b - float(ib);
      }
    }
    for (int x = 0; x < w; ++x) cov[size_t(y) * w + x] = std::min(row[x] / kSub, 1.0f);
  }
  return cov;
}

// The outlines of `f` at `level`, vectorized, filled at f's own size.
std::vector<float> VectorCoverage(const std::vector<float>& f, int w, int h, float level,
                                  float scale) {
  std::vector<Loop> shapes;
  for (Loop& l : TraceContours(f, w, h, level)) {
    // Specks (DXT noise) out; field sample i is pixel i's centre.
    if (LoopArea(l) < 0.3f * scale * scale) continue;
    for (Pt& p : l) {
      p.x += 0.5f;
      p.y += 0.5f;
    }
    // Simplified to within 0.3 source pixels; turns sharper than 50 degrees
    // stay corners.
    shapes.push_back(Vectorize(l, 0.3f * scale, std::cos(50.0f * 3.14159265f / 180.0f)));
  }
  return FillLoops(shapes, w, h);
}
}  // namespace

std::vector<uint8_t> UpscaleGlyphAtlas(const uint8_t* rgba, uint32_t w32, uint32_t h32,
                                       uint32_t scale) {
  const int w = int(w32), h = int(h32), S = int(scale);
  const int ow = w * S, oh = h * S;
  const size_t n = size_t(w) * h;
  // Source fields: alpha, luma (of the visible texels), premultiplied colour.
  std::vector<float> a(n), luma(n), light(n), rgb(n * 3), prem(n * 3);
  std::vector<float> er(n), edge_w(n), edge_prem(n * 3);
  for (size_t i = 0; i < n; ++i) a[i] = rgba[i * 4 + 3] / 255.0f;
  // The shape one texel in from its edge (3x3 minimum of alpha): the edge
  // texels are the art's rim (a light ring on the button icons, the outline's
  // edge on text), traced as a band of its own so it stays an even line.
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float m = 1.0f;
      for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
          const int xx = x + i, yy = y + j;
          m = std::min(m, xx < 0 || yy < 0 || xx >= w || yy >= h ? 0.0f : a[size_t(yy) * w + xx]);
        }
      er[size_t(y) * w + x] = m;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) {
      rgb[i * 3 + c] = rgba[i * 4 + c] / 255.0f;
      // Weighted toward opaque texels: the semi-transparent edge texels'
      // colours (often a light fringe from the DXT blocks) count little.
      prem[i * 3 + c] = rgb[i * 3 + c] * a[i] * a[i] * a[i];
    }
    // Semi-transparent texels are the art's anti-aliased edge, often with a
    // light fringe (drawn over white): their colour isn't the shape's.
    luma[i] = a[i] > 0.8f ? Luma(&rgb[i * 3]) : 0.0f;
    const float solid = std::clamp((a[i] - 0.6f) / 0.3f, 0.0f, 1.0f);
    const float inner = std::clamp((er[i] - 0.6f) / 0.3f, 0.0f, 1.0f);
    light[i] = Luma(&rgb[i * 3]) * solid * inner;  // the light shape: lit, solid, inside
    edge_w[i] = a[i] * (1.0f - inner);
    for (int c = 0; c < 3; ++c) edge_prem[i * 3 + c] = rgb[i * 3 + c] * edge_w[i];
  }
  // A light blur first: the edges are then found from the bitmap's overall
  // shape rather than its pixel steps (and the DXT blocks' noise).
  // The outer shape (alpha, noisier: DXT3 keeps 4 bits of it) a little more
  // than the light shape inside.
  const std::vector<float> ab = Blur(a, w, h, 0.6f);
  const std::vector<float> lb = Blur(light, w, h, 0.45f);
  const std::vector<float> eb = Blur(er, w, h, 0.6f);

  // Pass 1: the blurred alpha and luma, and the colour, at every new pixel.
  const size_t on = size_t(ow) * oh;
  std::vector<float> A(on), L(on), C(on * 3), E(on), F(on * 3, -1.0f);
  std::vector<int> ix(size_t(ow) * 4), iy(size_t(oh) * 4);
  std::vector<float> wx(size_t(ow) * 4), wy(size_t(oh) * 4);
  for (int x = 0; x < ow; ++x) {
    const float sx = (x + 0.5f) / S - 0.5f;
    const float fx = std::floor(sx);
    CubicWeights(sx - fx, &wx[size_t(x) * 4]);
    for (int k = 0; k < 4; ++k) ix[size_t(x) * 4 + k] = std::clamp(int(fx) - 1 + k, 0, w - 1);
  }
  for (int y = 0; y < oh; ++y) {
    const float sy = (y + 0.5f) / S - 0.5f;
    const float fy = std::floor(sy);
    CubicWeights(sy - fy, &wy[size_t(y) * 4]);
    for (int k = 0; k < 4; ++k) iy[size_t(y) * 4 + k] = std::clamp(int(fy) - 1 + k, 0, h - 1);
  }
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      float va = 0, vl = 0, vu = 0, vc[3] = {0, 0, 0}, ve = 0, vw = 0, vf[3] = {0, 0, 0};
      for (int j = 0; j < 4; ++j) {
        const size_t row = size_t(iy[size_t(y) * 4 + j]) * w;
        const float wyj = wy[size_t(y) * 4 + j];
        for (int i = 0; i < 4; ++i) {
          const size_t s = row + ix[size_t(x) * 4 + i];
          const float wt = wyj * wx[size_t(x) * 4 + i];
          va += wt * ab[s];
          vl += wt * lb[s];
          vu += wt * a[s] * a[s] * a[s];
          vc[0] += wt * prem[s * 3];
          vc[1] += wt * prem[s * 3 + 1];
          vc[2] += wt * prem[s * 3 + 2];
          ve += wt * eb[s];
          vw += wt * edge_w[s];
          for (int c = 0; c < 3; ++c) vf[c] += wt * edge_prem[s * 3 + c];
        }
      }
      const size_t o = size_t(y) * ow + x;
      A[o] = va;
      L[o] = vl;
      const float inv = 1.0f / std::max(vu, 1e-3f);
      for (int c = 0; c < 3; ++c) C[o * 3 + c] = std::clamp(vc[c] * inv, 0.0f, 1.0f);
      E[o] = ve;
      if (vw > 0.05f) {
        for (int c = 0; c < 3; ++c) F[o * 3 + c] = std::clamp(vf[c] / vw, 0.0f, 1.0f);
      }
    }
  }

  // Pass 2: the shapes (opaque, and light within it) as vector outlines,
  // filled with exact anti-aliasing at the new size: straight edges and sharp
  // corners as a font would have, not the bitmap's pixel steps.
  const std::vector<float> cover_a = VectorCoverage(A, ow, oh, 0.5f, float(S));
  const std::vector<float> cover_l = VectorCoverage(L, ow, oh, 0.5f, float(S));
  const std::vector<float> cover_e = VectorCoverage(E, ow, oh, 0.5f, float(S));
  std::vector<uint8_t> out(on * 4);
  for (int y = 0; y < oh; ++y) {
    const float sy = (y + 0.5f) / S - 0.5f;
    const int by = int(std::floor(sy));
    for (int x = 0; x < ow; ++x) {
      const size_t o = size_t(y) * ow + x;
      const float alpha = cover_a[o];
      // Two-tone areas (white text in a black outline): the lightest and
      // darkest visible texels nearby, split along the light shape's outline.
      const float sx = (x + 0.5f) / S - 0.5f;
      const int bx = int(std::floor(sx));
      float hi_l = -1.0f, lo_l = 2.0f;
      const float *hi = nullptr, *lo = nullptr;
      for (int j = -1; j <= 2; ++j) {
        const int yy = by + j;
        if (yy < 0 || yy >= h) continue;
        for (int i = -1; i <= 2; ++i) {
          const int xx = bx + i;
          if (xx < 0 || xx >= w) continue;
          const size_t s = size_t(yy) * w + xx;
          if (a[s] <= 0.8f) continue;
          const float l = luma[s];
          if (l > hi_l) hi_l = l, hi = &rgb[s * 3];
          if (l < lo_l) lo_l = l, lo = &rgb[s * 3];
        }
      }
      float col[3] = {C[o * 3], C[o * 3 + 1], C[o * 3 + 2]};
      // (Light near white and dark near black, as the light shape is traced
      // halfway; other tones, such as an icon's grey rim, are interpolated.)
      if (hi && lo && hi_l > 0.75f && lo_l < 0.25f && Saturation(hi) < 0.2f &&
          Saturation(lo) < 0.2f) {
        const float t = cover_l[o];
        for (int c = 0; c < 3; ++c) col[c] = lo[c] + (hi[c] - lo[c]) * t;
      }
      // The rim band: between the outer outline and the one a texel in.
      if (F[o * 3] >= 0.0f) {
        const float t = cover_e[o];
        for (int c = 0; c < 3; ++c) col[c] = F[o * 3 + c] + (col[c] - F[o * 3 + c]) * t;
      }
      for (int c = 0; c < 3; ++c) out[o * 4 + c] = uint8_t(std::lround(col[c] * 255.0f));
      out[o * 4 + 3] = uint8_t(std::lround(alpha * 255.0f));
    }
  }
  return out;
}

std::vector<ReplacementLevel> BuildMips(ReplacementLevel base) {
  std::vector<ReplacementLevel> levels;
  levels.push_back(std::move(base));
  while (levels.back().width > 1 || levels.back().height > 1) {
    const ReplacementLevel& p = levels.back();
    ReplacementLevel l;
    l.width = std::max(1u, p.width / 2);
    l.height = std::max(1u, p.height / 2);
    l.rgba.resize(size_t(l.width) * l.height * 4);
    for (uint32_t y = 0; y < l.height; ++y) {
      for (uint32_t x = 0; x < l.width; ++x) {
        float acc[4] = {0, 0, 0, 0};
        for (uint32_t j = 0; j < 2; ++j) {
          for (uint32_t i = 0; i < 2; ++i) {
            const uint32_t sx = std::min(x * 2 + i, p.width - 1);
            const uint32_t sy = std::min(y * 2 + j, p.height - 1);
            const uint8_t* s = &p.rgba[(size_t(sy) * p.width + sx) * 4];
            const float al = s[3] / 255.0f;
            for (int c = 0; c < 3; ++c) acc[c] += s[c] * al;
            acc[3] += al;
          }
        }
        uint8_t* d = &l.rgba[(size_t(y) * l.width + x) * 4];
        for (int c = 0; c < 3; ++c) {
          d[c] = acc[3] > 0.0f ? uint8_t(std::lround(std::min(acc[c] / acc[3], 255.0f))) : 0;
        }
        d[3] = uint8_t(std::lround(acc[3] / 4.0f * 255.0f));
      }
    }
    levels.push_back(std::move(l));
  }
  return levels;
}

TextureReplacer::TextureReplacer(const std::string& folder, bool texture_pack, bool hd_fonts,
                                 const std::string& cache_folder, bool upscaled)
    : hd_fonts_(hd_fonts), cache_folder_(cache_folder) {
  if (folder.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return;
  const auto opts = std::filesystem::directory_options::skip_permission_denied;
  if (texture_pack) {
    // Everything but the upscaler's folder.
    for (auto it = std::filesystem::recursive_directory_iterator(folder, opts, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_directory(ec)) {
        if (it.depth() == 0 && it->path().filename() == kUpscaledFolder) it.disable_recursion_pending();
        continue;
      }
      uint64_t hash;
      if (it->is_regular_file(ec) && ParsePackName(it->path(), &hash)) pack_[hash] = it->path().string();
    }
  }
  const auto up = std::filesystem::path(folder) / kUpscaledFolder;
  if (upscaled && std::filesystem::is_directory(up, ec)) {
    for (auto it = std::filesystem::recursive_directory_iterator(up, opts, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      uint64_t hash;
      if (it->is_regular_file(ec) && ParsePackName(it->path(), &hash)) upscaled_[hash] = it->path().string();
    }
  }
}

bool TextureReplacer::MayReplace(const TextureFetch& tf) const {
  if (tf.dimension() != 1 || !BcFormatFor(tf.format())) return false;
  if (!pack_.empty() || !upscaled_.empty()) return true;
  // Only the fonts: all DXT3, 256 or 512 square or 256x512.
  return hd_fonts_ && tf.format() == 0x13 && (tf.width() == 256 || tf.width() == 512) &&
         (tf.height() == 256 || tf.height() == 512);
}

uint64_t TextureReplacer::ContentHash(const TextureFetch& tf, const uint8_t* data, size_t size) {
  const uint32_t bc = BcFormatFor(tf.format());
  if (!bc || !data) return 0;
  const size_t n = size_t((tf.width() + 3) / 4) * ((tf.height() + 3) / 4) * BlockBytes(bc);
  if (size < n) return 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

std::shared_ptr<const Replacement> TextureReplacer::Find(const TextureFetch& tf, const uint8_t* data,
                                                         size_t size) {
  if (!MayReplace(tf)) return nullptr;
  const uint64_t hash = ContentHash(tf, data, size);
  if (!hash) return nullptr;
  // The pack first (hand-made), then a rebuilt font, then the upscaler's.
  const std::string* png = nullptr;
  bool from_upscaler = false;
  if (auto pit = pack_.find(hash); pit != pack_.end()) png = &pit->second;
  const KnownFont* font = !png && hd_fonts_ ? FontFor(hash) : nullptr;
  if (!png && !font) {
    if (auto uit = upscaled_.find(hash); uit != upscaled_.end()) {
      png = &uit->second;
      from_upscaler = true;
    }
  }
  if (!png && !font) return nullptr;

  std::lock_guard lock(mutex_);
  if (auto it = loaded_.find(hash); it != loaded_.end()) return it->second;
  if (failed_.count(hash)) return nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  auto rep = std::make_shared<Replacement>();
  char hex[20];
  std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(hash));
  if (png) {
    // A PNG from the pack (it wins over a rebuilt font) or the upscaler.
    int pw = 0, ph = 0, comp = 0;
    stbi_uc* px = nullptr;
    {
      if (FILE* f = OpenFile(*png, "rb")) {
        px = stbi_load_from_file(f, &pw, &ph, &comp, 4);
        std::fclose(f);
      }
    }
    if (!px || pw <= 0 || ph <= 0) {
      messages_.push_back("texture pack: couldn't read " + *png +
                          (px ? "" : std::string(" (") + stbi_failure_reason() + ")"));
      if (px) stbi_image_free(px);
      failed_[hash] = true;
      return nullptr;
    }
    ReplacementLevel base;
    base.width = uint32_t(pw);
    base.height = uint32_t(ph);
    base.rgba.assign(px, px + size_t(pw) * ph * 4);
    stbi_image_free(px);
    rep->levels = BuildMips(std::move(base));
    rep->swizzled = false;
    rep->source = (from_upscaler ? "upscaled " : "") + *png;
  } else {
    // A font: rebuilt from its own glyphs, as the renderer would show them.
    auto decoded = DecodeTextureMips(tf, data, size, nullptr, 0, false);
    if (decoded.empty() || !decoded[0].note.empty() || decoded[0].bc ||
        decoded[0].rgba.size() != size_t(decoded[0].width) * decoded[0].height * 4) {
      messages_.push_back(std::string("sharper fonts: couldn't decode ") + font->name);
      failed_[hash] = true;
      return nullptr;
    }
    ReplacementLevel base;
    base.width = decoded[0].width * kFontScale;
    base.height = decoded[0].height * kFontScale;
    const std::string cache = FontCachePath(cache_folder_, hash);
    const bool cached = LoadFontCache(cache, base.width, base.height, &base.rgba);
    if (!cached) {
      base.rgba = UpscaleGlyphAtlas(decoded[0].rgba.data(), decoded[0].width, decoded[0].height,
                                    kFontScale);
      SaveFontCache(cache, base.width, base.height, base.rgba);
    }
    rep->levels = BuildMips(std::move(base));
    rep->swizzled = true;
    rep->source = std::string("font ") + font->name + " at " + std::to_string(kFontScale) + "x" +
                  (cached ? " (cached)" : "");
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char line[160];
  std::snprintf(line, sizeof(line), "replaced texture %s (%ux%u) with %ux%u: ", hex, tf.width(),
                tf.height(), rep->levels[0].width, rep->levels[0].height);
  messages_.push_back(line + rep->source + " (" + std::to_string(int(ms)) + " ms)");
  loaded_[hash] = rep;
  return rep;
}

bool TextureReplacer::IsFont(uint64_t hash) { return FontFor(hash) != nullptr; }

std::vector<std::string> TextureReplacer::TakeMessages() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  out.swap(messages_);
  return out;
}

}  // namespace amped3::native
