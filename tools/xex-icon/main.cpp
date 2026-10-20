// amped3-icon: the game's Xbox 360 dashboard icon (the square Amped 3 logo,
// the title image in default.xex's XDBF resource), as the icon for the
// shortcuts the installers and the launcher make. Run at install time on the
// user's own copy of the game: no game art is shipped with the project.
//
//   amped3-icon <default.xex> <out folder>
//
// Writes amped3-icon.png (256 x 256, for Linux desktop entries and Steam),
// amped3-icon.ico (16 to 256 pixels, for Windows shortcuts) and
// amped3-icon-64.png (the image exactly as on the console). The XEX's image
// is decrypted (AES-128, retail key) and decompressed (LZX) as the SDK's
// loader does; tiny-AES-c and libmspack's LZX decoder come from the ReXGlue
// SDK checkout.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <aes.h>
#include <lzx.h>
#include <mspack.h>
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

bool Fail(const std::string& message) {
  std::fprintf(stderr, "amped3-icon: %s\n", message.c_str());
  return false;
}

// --- LZX through libmspack, in memory ---------------------------------------
struct MemFile {
  uint8_t* data;
  size_t size, offset;
};
int MemRead(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t k = std::min(size_t(n), m->size - m->offset);
  std::memcpy(buf, m->data + m->offset, k);
  m->offset += k;
  return int(k);
}
int MemWrite(mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t k = std::min(size_t(n), m->size - m->offset);
  std::memcpy(m->data + m->offset, buf, k);
  m->offset += k;
  return int(k);
}
void* MemAlloc(mspack_system*, size_t n) { return std::calloc(n, 1); }
void MemFree(void* p) { std::free(p); }
void MemCopy(void* src, void* dst, size_t n) { std::memcpy(dst, src, n); }

bool Lzx(Bytes& in, Bytes& out, uint32_t window) {
  int bits = 0;
  while ((1u << bits) < window) ++bits;
  mspack_system sys{};
  sys.read = MemRead;
  sys.write = MemWrite;
  sys.alloc = MemAlloc;
  sys.free = MemFree;
  sys.copy = MemCopy;
  MemFile src{in.data(), in.size(), 0}, dst{out.data(), out.size(), 0};
  lzxd_stream* s = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&src),
                             reinterpret_cast<mspack_file*>(&dst), bits, 0, 0x8000,
                             off_t(out.size()), 0);
  if (!s) return false;
  const int rc = lzxd_decompress(s, off_t(out.size()));
  lzxd_free(s);
  return rc == MSPACK_ERR_OK;
}

// --- The XEX's image ----------------------------------------------------------
struct Xex {
  uint32_t base = 0;
  Bytes image;
  std::vector<std::pair<uint32_t, uint32_t>> resources;  // address, size
};

bool LoadXex(const Bytes& x, Xex* out) {
  if (x.size() < 0x18 || std::memcmp(x.data(), "XEX2", 4) != 0) return Fail("not an XEX2 file");
  const uint32_t data_offset = Be32(&x[0x08]);
  const uint32_t security = Be32(&x[0x10]);
  const uint32_t count = Be32(&x[0x14]);
  if (data_offset > x.size() || security + 0x160 > x.size()) return Fail("truncated XEX header");
  uint32_t format = 0, base = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 0x18 + i * 8;
    if (at + 8 > x.size()) return Fail("truncated XEX header");
    const uint32_t key = Be32(&x[at]), value = Be32(&x[at + 4]);
    if (key == 0x000003FF) format = value;
    if (key == 0x00010201) base = value;
    if (key == 0x000002FF && value + 4 <= x.size()) {
      const uint32_t size = Be32(&x[value]);
      for (uint32_t r = 0; r + 1 <= (size - 4) / 16; ++r) {
        const size_t e = value + 4 + r * 16;
        if (e + 16 > x.size()) break;
        out->resources.push_back({Be32(&x[e + 8]), Be32(&x[e + 12])});
      }
    }
  }
  if (!format) return Fail("no file format header");
  const uint32_t image_size = Be32(&x[security + 4]);
  out->base = base ? base : Be32(&x[security + 0x110]);
  const uint16_t encryption = Be16(&x[format + 4]);
  const uint16_t compression = Be16(&x[format + 6]);

  Bytes data(x.begin() + data_offset, x.end());
  if (encryption == 1) {
    // The file key, decrypted with the retail key, is the session key.
    static const uint8_t kRetail[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                        0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
    uint8_t session[16];
    std::memcpy(session, &x[security + 0x150], 16);
    AES_ctx ctx;
    AES_init_ctx(&ctx, kRetail);
    AES_ECB_decrypt(&ctx, session);
    const uint8_t iv[16] = {};
    AES_init_ctx_iv(&ctx, session, iv);
    data.resize(data.size() & ~size_t(15));
    AES_CBC_decrypt_buffer(&ctx, data.data(), data.size());
  } else if (encryption != 0) {
    return Fail("unknown encryption");
  }

  out->image.assign(image_size, 0);
  if (compression == 0) {
    std::memcpy(out->image.data(), data.data(), std::min<size_t>(data.size(), image_size));
  } else if (compression == 1) {
    // Blocks of (data, zeros).
    const uint32_t blocks = (Be32(&x[format]) - 8) / 8;
    size_t src = 0, dst = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
      const uint32_t n = Be32(&x[format + 8 + b * 8]), zeros = Be32(&x[format + 12 + b * 8]);
      if (src + n > data.size() || dst + n + zeros > image_size) return Fail("bad basic compression");
      std::memcpy(&out->image[dst], &data[src], n);
      src += n;
      dst += n + zeros;
    }
  } else if (compression == 2) {
    // Blocks: next block's size and hash, then chunks (2-byte size) of LZX.
    const uint32_t window = Be32(&x[format + 8]);
    uint32_t block_size = Be32(&x[format + 12]);
    Bytes lzx;
    size_t p = 0;
    while (block_size) {
      if (p + block_size > data.size()) return Fail("bad compressed block (wrong key?)");
      const size_t next = p + block_size;
      const uint32_t next_size = Be32(&data[p]);
      size_t q = p + 24;
      while (q + 2 <= next) {
        const uint16_t n = Be16(&data[q]);
        q += 2;
        if (!n) break;
        if (q + n > next) return Fail("bad compressed chunk");
        lzx.insert(lzx.end(), data.begin() + q, data.begin() + q + n);
        q += n;
      }
      p = next;
      block_size = next_size;
    }
    if (!Lzx(lzx, out->image, window)) return Fail("LZX decompression failed");
  } else {
    return Fail("unknown compression");
  }
  return true;
}

// --- XDBF: the title image ------------------------------------------------------
bool TitleImage(const Xex& xex, Bytes* png) {
  for (const auto& [address, size] : xex.resources) {
    if (address < xex.base || address - xex.base + size > xex.image.size()) continue;
    const uint8_t* d = &xex.image[address - xex.base];
    if (size < 0x18 || std::memcmp(d, "XDBF", 4) != 0) continue;
    const uint32_t entry_table = Be32(d + 8), entries = Be32(d + 12), free_table = Be32(d + 16);
    const size_t header = 0x18, data = header + size_t(entry_table) * 0x12 + size_t(free_table) * 8;
    for (uint32_t i = 0; i < entries && header + (i + 1) * 0x12 <= size; ++i) {
      const uint8_t* e = d + header + i * 0x12;
      // Namespace 2 (images), id 0x8000: the title's dashboard image.
      if (Be16(e) != 2 || Be64(e + 2) != 0x8000) continue;
      const uint32_t offset = Be32(e + 10), length = Be32(e + 14);
      if (data + offset + length > size) return Fail("bad XDBF entry");
      png->assign(d + data + offset, d + data + offset + length);
      return true;
    }
  }
  return Fail("no title image in the XEX's resources");
}

// --- Resizing ---------------------------------------------------------------------
using Rgba = std::vector<float>;  // premultiplied

// Separable resampling: box (area) when shrinking, Catmull-Rom when growing.
Rgba Resize(const Rgba& src, int sw, int sh, int dw, int dh) {
  auto pass = [](const Rgba& in, int w, int h, int nw, bool horizontal) {
    const int n_in = horizontal ? w : h, n_out = nw;
    const int other = horizontal ? h : w;
    Rgba out(size_t(horizontal ? nw * h : w * nw) * 4, 0.0f);
    const double scale = double(n_in) / n_out;
    for (int o = 0; o < n_out; ++o) {
      const double center = (o + 0.5) * scale - 0.5;
      std::vector<std::pair<int, double>> taps;
      if (scale > 1.0) {  // area
        const double lo = o * scale, hi = (o + 1) * scale;
        for (int i = int(lo); i < int(std::ceil(hi)); ++i) {
          const double wgt = std::min(hi, i + 1.0) - std::max(lo, double(i));
          if (wgt > 0) taps.push_back({std::clamp(i, 0, n_in - 1), wgt});
        }
      } else {  // Catmull-Rom
        const int c = int(std::floor(center));
        for (int i = c - 1; i <= c + 2; ++i) {
          const double t = std::abs(center - i);
          const double wgt = t < 1 ? 1.5 * t * t * t - 2.5 * t * t + 1
                             : t < 2 ? -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2
                                     : 0.0;
          taps.push_back({std::clamp(i, 0, n_in - 1), wgt});
        }
      }
      double sum = 0;
      for (auto& t : taps) sum += t.second;
      for (int k = 0; k < other; ++k) {
        float acc[4] = {};
        for (auto& [i, wgt] : taps) {
          const size_t at = horizontal ? (size_t(k) * w + i) * 4 : (size_t(i) * w + k) * 4;
          for (int ch = 0; ch < 4; ++ch) acc[ch] += float(in[at + ch] * wgt / sum);
        }
        const size_t to = horizontal ? (size_t(k) * nw + o) * 4 : (size_t(o) * w + k) * 4;
        for (int ch = 0; ch < 4; ++ch) out[to + ch] = acc[ch];
      }
    }
    return out;
  };
  const Rgba h = pass(src, sw, sh, dw, true);
  return pass(h, dw, sh, dh, false);
}

std::vector<uint8_t> ToBytes(const Rgba& img) {
  std::vector<uint8_t> out(img.size());
  for (size_t i = 0; i < img.size(); i += 4) {
    const float a = std::clamp(img[i + 3], 0.0f, 1.0f);
    for (int ch = 0; ch < 3; ++ch) {
      const float v = a > 0 ? img[i + ch] / a : 0.0f;
      out[i + ch] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
    out[i + 3] = uint8_t(std::lround(a * 255.0f));
  }
  return out;
}

Bytes Png(const std::vector<uint8_t>& rgba, int w, int h) {
  Bytes out;
  stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        auto* o = static_cast<Bytes*>(ctx);
        o->insert(o->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
      },
      &out, w, h, 4, rgba.data(), w * 4);
  return out;
}

bool WriteFile(const fs::path& path, const Bytes& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(f) || Fail("couldn't write " + path.string());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: amped3-icon <default.xex> <out folder>\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) return Fail(std::string("couldn't open ") + argv[1]), 1;
  const Bytes x((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Xex xex;
  Bytes png;
  if (!LoadXex(x, &xex) || !TitleImage(xex, &png)) return 1;

  int w = 0, h = 0, n = 0;
  uint8_t* px = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
  if (!px) return Fail("the title image isn't a PNG"), 1;
  Rgba src(size_t(w) * h * 4);
  for (size_t i = 0; i < src.size(); i += 4) {
    const float a = px[i + 3] / 255.0f;
    for (int ch = 0; ch < 3; ++ch) src[i + ch] = px[i + ch] / 255.0f * a;
    src[i + 3] = a;
  }
  stbi_image_free(px);

  const fs::path out = argv[2];
  std::error_code ec;
  fs::create_directories(out, ec);
  // ICO: one PNG per size (Windows Vista and later read PNG entries).
  const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
  std::vector<Bytes> images;
  for (int s : sizes) images.push_back(s == w && s == h ? png : Png(ToBytes(Resize(src, w, h, s, s)), s, s));
  Bytes ico = {0, 0, 1, 0, uint8_t(std::size(sizes)), 0};
  uint32_t offset = 6 + 16 * uint32_t(std::size(sizes));
  for (size_t i = 0; i < images.size(); ++i) {
    const int s = sizes[i];
    const uint32_t len = uint32_t(images[i].size());
    const uint8_t entry[16] = {uint8_t(s & 255), uint8_t(s & 255), 0, 0, 1, 0, 32, 0,
                               uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24),
                               uint8_t(offset), uint8_t(offset >> 8), uint8_t(offset >> 16),
                               uint8_t(offset >> 24)};
    ico.insert(ico.end(), entry, entry + 16);
    offset += len;
  }
  for (const Bytes& b : images) ico.insert(ico.end(), b.begin(), b.end());

  if (!WriteFile(out / "amped3-icon.ico", ico) || !WriteFile(out / "amped3-icon.png", images.back()) ||
      !WriteFile(out / "amped3-icon-64.png", png)) {
    return 1;
  }
  std::printf("Icon (%dx%d title image) written to %s\n", w, h, out.string().c_str());
  return 0;
}
