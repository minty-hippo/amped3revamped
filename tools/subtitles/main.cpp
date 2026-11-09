// amped3-subtitles: makes subtitles for the game's dialogue on the player's PC.
//
// Finds the game's sounds in its own files (XACT wave banks, .xwb, and RIFF
// XMA files, loose or inside the .res archives), decodes their XMA audio
// (the ReXGlue SDK's FFmpeg, the same XMA decoder the game plays them with),
// and writes down what's said in them with offline speech recognition
// (whisper.cpp; the model is downloaded by the installer). The result is
// <out>/amped3.subtitles (src/subtitle_file.h), which the game reads to show
// subtitles while a recognised sound plays (src/subtitles.cpp).
//
// Nothing of the game leaves the PC and nothing of it ships with the project:
// the file is made from the player's own copy.
//
// Which sounds: music is left out (long stereo sounds and banks of them, and
// anything named like music), so are sounds too short to hold a word. What
// isn't speech comes out empty (whisper's no-speech estimate, and sounds it
// only hears as noises are dropped).
//
// Speed: short sounds are put together, a couple of seconds of silence
// between them, into windows of up to ~25 s (whisper always listens in 30 s
// windows), and each word is given back to the sound it falls in. Progress is
// kept in <out>/work/transcripts.cache, so a run that's stopped carries on
// where it left off.
//
//   amped3-subtitles --game "<install>\game" --out "<install>\subtitles"
//       --model "<folder>\ggml-base.en.bin" [--threads N] [--force]
//       [--scan] [--dump-wav <folder>] [--include-music] [--limit N]
//
//   --scan          only list what's found (<out>/scan.txt), no transcribing
//   --dump-wav      also write each decoded sound as a 16 kHz WAV (checking)
//   --include-music transcribe long stereo sounds too
//   --limit N       stop after N sounds (testing)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gui_patch_core.h"
#include "subtitle_file.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
}

#include "whisper.h"

namespace fs = std::filesystem;
namespace sf = amped3::subtitle_file;
using Bytes = std::vector<uint8_t>;

namespace {

// ---- small helpers -----------------------------------------------------------

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool ReadFile(const fs::path& p, Bytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  if (size < 0) return false;
  out->resize(size_t(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size()));
  return bool(f) || f.eof();
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t Le32(const uint8_t* p) {
  return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

std::string Printable(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s += (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '.';
  return s;
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02X", p[i]);
    s += b;
  }
  return s;
}

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool IsStandardRate(uint32_t r) {
  for (uint32_t s : {8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u}) {
    if (r == s) return true;
  }
  return false;
}

// ---- what's found ----------------------------------------------------------------

struct Sound {
  std::string source;     // "<file>" or "<file>:<entry>" or "<bank> #<index>"
  std::string container;  // the file (or archive entry) it's in
  int channels = 0;
  int rate = 0;
  Bytes data;             // XMA packets
  bool in_bank = false;
  std::string bank;       // wave bank name (when in_bank)
};

struct Scan {
  std::vector<Sound> sounds;
  std::vector<std::string> report;  // scan.txt lines
  std::set<std::string> video_stems;  // lowercased file stems of movie files
  std::map<std::string, int> other_kinds;  // ext -> count, for files with no sounds
  std::map<std::string, std::string> other_magic;  // ext -> first bytes seen
};

// XACT wave bank (.xwb). Little-endian "WBND", or big-endian (Xbox 360,
// "DNBW" as bytes). Layouts differ between XACT versions; the header is
// checked for the one that fits rather than going by version numbers.
void FromWaveBank(const std::string& name, const Bytes& d, Scan* scan) {
  if (d.size() < 64) return;
  const bool be = std::memcmp(d.data(), "DNBW", 4) == 0;
  auto u32 = [&](size_t o) -> uint32_t {
    if (o + 4 > d.size()) return 0;
    return be ? Be32(&d[o]) : Le32(&d[o]);
  };
  const uint32_t version = u32(4);
  // Segment table at 8 (no header version) or 12; 4 or 5 segments. The bank
  // data segment comes straight after it.
  size_t seg_base = 0;
  int segs = 0;
  for (size_t base : {size_t(12), size_t(8)}) {
    for (int n : {5, 4}) {
      if (u32(base) == base + size_t(n) * 8 && u32(base + 4) >= 24 && u32(base + 4) < 4096) {
        seg_base = base;
        segs = n;
        break;
      }
    }
    if (segs) break;
  }
  if (!segs) {
    scan->report.push_back("  wave bank " + name + ": header layout not recognised (version " +
                           std::to_string(version) + ")");
    return;
  }
  auto seg_off = [&](int i) { return u32(seg_base + size_t(i) * 8); };
  auto seg_len = [&](int i) { return u32(seg_base + size_t(i) * 8 + 4); };
  const size_t bank = seg_off(0);
  const uint32_t flags = u32(bank);
  const uint32_t count = u32(bank + 4);
  // Bank name 64 characters (XACT 2/3) or 16 (older).
  size_t name_len = 64;
  uint32_t meta_size = u32(bank + 8 + 64);
  if (meta_size < 16 || meta_size > 64) {
    name_len = 16;
    meta_size = u32(bank + 8 + 16);
  }
  const uint32_t alignment = u32(bank + 8 + name_len + 8);
  const uint32_t compact_format = u32(bank + 8 + name_len + 12);
  std::string bank_name;
  for (size_t i = 0; i < name_len && bank + 8 + i < d.size() && d[bank + 8 + i]; ++i) {
    bank_name += char(d[bank + 8 + i]);
  }
  const bool compact = (flags & 0x00020000) != 0;
  const int meta_seg = 1;
  const int data_seg = segs == 5 ? 4 : 3;
  const size_t meta = seg_off(meta_seg);
  const size_t wave_data = seg_off(data_seg);
  const size_t wave_len = seg_len(data_seg);
  char line[256];
  std::snprintf(line, sizeof(line),
                "  wave bank %s ('%s'): XACT version %u, %u entries, %s, %s, entry size %u",
                name.c_str(), bank_name.c_str(), version, count,
                (flags & 1) ? "streaming" : "in memory", be ? "big-endian" : "little-endian",
                compact ? 4u : meta_size);
  scan->report.push_back(line);
  if (count == 0 || count > 200000 || wave_data >= d.size() + 1) return;

  // MINIWAVEFORMAT: tag 2 bits (1 = XMA), channels 3, rate 18 (26 in older
  // banks), block align 8, bits 1. Xbox 360 banks may store the fields in
  // the other bit order; both are tried.
  struct Format {
    int tag, channels;
    uint32_t rate;
  };
  auto decode_format = [](uint32_t f, Format* out) {
    Format a{int(f & 3), int((f >> 2) & 7), (f >> 5) & 0x3FFFF};
    Format a26{int(f & 3), int((f >> 2) & 7), (f >> 5) & 0x3FFFFFF};
    Format b{int((f >> 30) & 3), int((f >> 27) & 7), (f >> 9) & 0x3FFFF};
    for (const Format& x : {a, b, a26}) {
      if (x.channels >= 1 && x.channels <= 6 && x.rate >= 8000 && x.rate <= 48000 &&
          (IsStandardRate(x.rate) || &x == &a26)) {
        *out = x;
        return true;
      }
    }
    for (const Format& x : {a, b, a26}) {
      if (x.channels >= 1 && x.channels <= 6 && x.rate >= 8000 && x.rate <= 48000) {
        *out = x;
        return true;
      }
    }
    return false;
  };

  int xma = 0, other = 0, bad = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t format = 0, offset = 0, length = 0;
    if (compact) {
      const uint32_t e = u32(meta + size_t(i) * 4);
      const uint32_t next = i + 1 < count ? u32(meta + size_t(i + 1) * 4) : 0;
      const uint32_t align = alignment ? alignment : 2048;
      offset = (e & 0x1FFFFF) * align;
      const uint32_t next_off = i + 1 < count ? (next & 0x1FFFFF) * align : uint32_t(wave_len);
      length = next_off > offset ? next_off - offset : 0;
      format = compact_format;
    } else {
      const size_t e = meta + size_t(i) * meta_size;
      format = u32(e + 4);
      offset = u32(e + 8);
      length = u32(e + 12);
    }
    Format f{};
    if (!decode_format(format, &f)) {
      ++bad;
      continue;
    }
    if (f.tag != 1) {
      ++other;
      continue;
    }
    if (size_t(offset) + length > wave_len || wave_data + offset + length > d.size() || length < 2048) {
      ++bad;
      continue;
    }
    Sound s;
    s.source = name + " #" + std::to_string(i);
    s.container = name;
    s.channels = f.channels;
    s.rate = int(f.rate);
    s.data.assign(d.begin() + std::ptrdiff_t(wave_data + offset),
                  d.begin() + std::ptrdiff_t(wave_data + offset + length));
    s.in_bank = true;
    s.bank = bank_name.empty() ? name : bank_name;
    scan->sounds.push_back(std::move(s));
    ++xma;
  }
  std::snprintf(line, sizeof(line), "    %d XMA sounds, %d in other formats (not used), %d unreadable",
                xma, other, bad);
  scan->report.push_back(line);
}

// RIFF WAVE with XMA (format 0x165 XMA1, 0x166 XMA2; or an XMA2 chunk).
// Xbox 360 files may have big-endian chunk sizes; the one that fits is used.
void FromRiff(const std::string& name, const Bytes& d, Scan* scan) {
  if (d.size() < 12 || std::memcmp(&d[8], "WAVE", 4) != 0) return;
  const bool rifx = std::memcmp(d.data(), "RIFX", 4) == 0;
  size_t pos = 12;
  int tag = 0, channels = 0, rate = 0;
  const uint8_t* data = nullptr;
  size_t data_len = 0;
  while (pos + 8 <= d.size()) {
    const uint8_t* c = &d[pos];
    uint32_t len = rifx ? Be32(c + 4) : Le32(c + 4);
    if (pos + 8 + len > d.size()) {
      const uint32_t other = rifx ? Le32(c + 4) : Be32(c + 4);
      len = pos + 8 + other <= d.size() ? other : uint32_t(d.size() - pos - 8);
    }
    const uint8_t* b = c + 8;
    if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
      // WAVEFORMATEX: tag, channels, rate (either byte order: the sensible one).
      const uint16_t tle = Le16(b), tbe = Be16(b);
      const bool le = tle == 0x165 || tle == 0x166 || tle == 1;
      tag = le ? tle : tbe;
      if (tag == 0x165 && len >= 32) {
        // XMAWAVEFORMAT: streams at 8, stream 0's rate at 16 and channels at 29.
        const int streams = le ? Le16(b + 8) : Be16(b + 8);
        rate = int(le ? Le32(b + 16) : Be32(b + 16));
        channels = 0;
        for (int s = 0; s < streams && 16 + size_t(s) * 20 + 14 <= len; ++s) channels += b[16 + s * 20 + 13];
      } else {
        channels = le ? Le16(b + 2) : Be16(b + 2);
        rate = int(le ? Le32(b + 4) : Be32(b + 4));
      }
    } else if (std::memcmp(c, "XMA2", 4) == 0 && len >= 16) {
      tag = 0x166;
      const int streams = b[1];
      rate = int(Be32(b + 12));
      channels = 0;
      for (int s = 0; s < streams && 52 + size_t(s) * 4 < len; ++s) channels += b[52 + s * 4];
      if (channels <= 0) channels = streams * 2;
    } else if (std::memcmp(c, "data", 4) == 0) {
      data = b;
      data_len = len;
    }
    pos += 8 + len + (len & 1);
  }
  if (!data || (tag != 0x165 && tag != 0x166)) {
    if (tag) scan->report.push_back("  " + name + ": RIFF WAVE, format " + std::to_string(tag) + " (not XMA; not used)");
    return;
  }
  if (channels < 1 || channels > 8 || rate < 8000 || rate > 48000) {
    scan->report.push_back("  " + name + ": XMA with a header that doesn't make sense (not used)");
    return;
  }
  Sound s;
  s.source = name;
  s.container = name;
  s.channels = channels;
  s.rate = rate;
  s.data.assign(data, data + data_len);
  scan->sounds.push_back(std::move(s));
}

bool LooksLikeVideo(const std::string& ext, const uint8_t* p, size_t n) {
  for (const char* e : {".vp6", ".flv", ".avi", ".wmv", ".bik", ".bk2", ".xmv", ".vid", ".mov", ".mp4",
                        ".sfd", ".usm", ".ivf", ".webm"}) {
    if (ext == e) return true;
  }
  if (n >= 3 && std::memcmp(p, "FLV", 3) == 0) return true;
  if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "AVI ", 4) == 0) return true;
  if (n >= 4 && std::memcmp(p, "DKIF", 4) == 0) return true;
  return false;
}

void FromData(const std::string& name, const std::string& ext, const Bytes& d, Scan* scan);

void FromZip(const std::string& name, const Bytes& d, Scan* scan) {
  std::vector<amped3::gui_patch::ZipEntry> entries;
  std::string error;
  if (!amped3::gui_patch::ReadZip(d, &entries, &error)) {
    scan->report.push_back("  " + name + ": archive not readable (" + error + ")");
    return;
  }
  for (auto& e : entries) {
    if (e.data.empty()) continue;
    const std::string ext = Lower(fs::path(e.name).extension().string());
    FromData(name + ":" + e.name, ext, e.data, scan);
    e.data.clear();
    e.data.shrink_to_fit();
    e.raw.clear();
    e.raw.shrink_to_fit();
  }
}

void FromData(const std::string& name, const std::string& ext, const Bytes& d, Scan* scan) {
  const size_t before = scan->sounds.size();
  if (d.size() >= 4 && (std::memcmp(d.data(), "WBND", 4) == 0 || std::memcmp(d.data(), "DNBW", 4) == 0)) {
    FromWaveBank(name, d, scan);
  } else if (d.size() >= 12 && (std::memcmp(d.data(), "RIFF", 4) == 0 || std::memcmp(d.data(), "RIFX", 4) == 0) &&
             std::memcmp(&d[8], "WAVE", 4) == 0) {
    FromRiff(name, d, scan);
  } else if (d.size() >= 4 && std::memcmp(d.data(), "PK\x03\x04", 4) == 0) {
    FromZip(name, d, scan);
    return;
  }
  if (LooksLikeVideo(ext, d.data(), d.size())) {
    // The leaf name's stem, so sounds named after a movie count as its track.
    std::string leaf = name.substr(name.find_last_of("/\\:") + 1);
    scan->video_stems.insert(Lower(fs::path(leaf).stem().string()));
    scan->report.push_back("  movie " + name + " (" + std::to_string(d.size() / 1024) + " KB, starts " +
                           Hex(d.data(), std::min<size_t>(16, d.size())) + " '" +
                           Printable(d.data(), std::min<size_t>(16, d.size())) + "')");
  } else if (scan->sounds.size() == before) {
    const std::string k = ext.empty() ? "(none)" : ext;
    if (scan->other_kinds[k]++ == 0) {
      scan->other_magic[k] = Hex(d.data(), std::min<size_t>(8, d.size())) + " '" +
                             Printable(d.data(), std::min<size_t>(8, d.size())) + "'";
    }
  }
}

// ---- XMA decoding ----------------------------------------------------------------
//
// XMA data is 2048-byte packets: a 32-bit header (frames that begin in it /
// sequence, the bit offset of the first frame that begins in it, metadata,
// and how many packets to skip to this stream's next one), then the frames'
// bits, which run on from packet to packet. Each frame starts with its length
// in bits (15 bits); its last bit says whether another frame follows in the
// same packet. Sounds with more than two channels are several streams (two
// channels each, the last one maybe one) whose packets are interleaved.
// Each frame goes to FFmpeg's xmaframes decoder (the XMA decoder the SDK
// plays the game's sound with) as one packet: a byte with the frame's bit
// padding, then the frame.

constexpr uint32_t kPacket = 2048;
constexpr uint32_t kPayloadBits = (kPacket - 4) * 8;

uint32_t FirstFrameOffset(const uint8_t* packet) {
  return ((uint32_t(packet[0] & 3) << 13) | (uint32_t(packet[1]) << 5) | (packet[2] >> 3)) + 32;
}

// Bits of the stream's packet payloads, joined.
struct Payload {
  Bytes bits;                    // MSB first
  std::vector<uint32_t> first;   // per packet: first frame's offset in its payload (bits), or ~0
  size_t total_bits() const { return bits.size() * 8; }
  uint32_t Peek(size_t pos, int n) const {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      const size_t b = pos + size_t(i);
      v = (v << 1) | ((b >> 3) < bits.size() ? (bits[b >> 3] >> (7 - (b & 7))) & 1 : 0);
    }
    return v;
  }
};

bool DecodeStream(const Payload& p, int channels, int rate, std::vector<float>* mono) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!codec) return false;
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) return false;
  ctx->sample_rate = rate;
  ctx->channels = channels;
  ctx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  Bytes buf(4096 + 64);
  const size_t packets = p.first.size();
  auto next_start = [&](size_t from_packet) -> size_t {
    for (size_t k = from_packet; k < packets; ++k) {
      if (p.first[k] != ~0u) return k * kPayloadBits + p.first[k];
    }
    return SIZE_MAX;
  };
  size_t pos = next_start(0);
  int errors = 0;
  size_t frames = 0;
  while (pos != SIZE_MAX && pos + 15 <= p.total_bits()) {
    const uint32_t size = p.Peek(pos, 15);
    if (size < 16 || size == 0x7FFF || pos + size > p.total_bits()) {
      pos = next_start(pos / kPayloadBits + 1);
      continue;
    }
    // The frame, with its byte's leading bits cleared and its last byte's
    // trailing bits.
    const size_t first_byte = pos >> 3;
    const uint32_t pad_start = uint32_t(pos & 7);
    const size_t bytes = (pad_start + size + 7) / 8;
    if (bytes + 1 > buf.size()) break;
    std::memset(buf.data(), 0, bytes + 2);
    std::memcpy(buf.data() + 1, &p.bits[first_byte], std::min(bytes, p.bits.size() - first_byte));
    buf[1] &= uint8_t(0xFF >> pad_start);
    const uint32_t used = (pad_start + size) & 7;
    if (used) buf[bytes] &= uint8_t(0xFF << (8 - used));
    const uint32_t pad_end = uint32_t(bytes * 8 - pad_start - size);
    buf[0] = uint8_t(((pad_start & 7) << 5) | ((pad_end & 7) << 2));
    pkt->data = buf.data();
    pkt->size = int(bytes + 1);
    if (avcodec_send_packet(ctx, pkt) >= 0) {
      while (avcodec_receive_frame(ctx, frame) >= 0) {
        const int n = frame->nb_samples;
        const size_t at = mono->size();
        mono->resize(at + size_t(n));
        for (int c = 0; c < std::min(channels, 2); ++c) {
          const float* s = reinterpret_cast<const float*>(frame->data[c]);
          if (!s) continue;
          for (int i = 0; i < n; ++i) (*mono)[at + size_t(i)] += s[i] / float(channels);
        }
        ++frames;
      }
    } else if (++errors > 64 && frames == 0) {
      break;
    }
    const bool more = p.Peek(pos + size - 1, 1) != 0;
    if (more) {
      pos += size;
    } else {
      pos = next_start(pos / kPayloadBits + 1);
    }
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  return frames > 0;
}

// The sound as mono floats at its own rate.
bool DecodeXma(const Sound& s, std::vector<float>* mono) {
  const size_t packets = s.data.size() / kPacket;
  if (!packets) return false;
  const int streams = std::max(1, (s.channels + 1) / 2);
  // Which stream each packet belongs to: stream k starts at the first packet
  // no earlier stream took, and goes on by its packets' skip counts.
  std::vector<int> owner(packets, -1);
  std::vector<std::vector<size_t>> list(static_cast<size_t>(streams));
  for (int k = 0; k < streams; ++k) {
    size_t at = 0;
    while (at < packets && owner[at] >= 0) ++at;
    while (at < packets) {
      if (owner[at] >= 0 && owner[at] != k) break;
      owner[at] = k;
      list[size_t(k)].push_back(at);
      const uint8_t skip = s.data[at * kPacket + 3];
      at += (skip == 0xFF ? 0 : skip) + 1;
    }
  }
  mono->clear();
  bool any = false;
  for (int k = 0; k < streams; ++k) {
    Payload p;
    p.bits.reserve(list[size_t(k)].size() * (kPacket - 4));
    for (size_t idx : list[size_t(k)]) {
      const uint8_t* packet = &s.data[idx * kPacket];
      const uint32_t off = FirstFrameOffset(packet);
      p.first.push_back(off >= 32 && off - 32 < kPayloadBits && packet[3] != 0xFF ? off - 32 : ~0u);
      p.bits.insert(p.bits.end(), packet + 4, packet + kPacket);
    }
    const int ch = (k < streams - 1 || s.channels % 2 == 0) ? 2 : 1;
    std::vector<float> one;
    if (!DecodeStream(p, std::min(ch, std::max(1, s.channels)), s.rate, &one)) continue;
    if (mono->size() < one.size()) mono->resize(one.size(), 0.0f);
    for (size_t i = 0; i < one.size(); ++i) (*mono)[i] += one[i];
    any = true;
  }
  return any;
}

// To 16 kHz (whisper's rate): box low-pass, then linear interpolation.
std::vector<float> To16k(const std::vector<float>& in, int rate) {
  if (rate == 16000 || in.empty()) return in;
  const double step = double(rate) / 16000.0;
  const int box = std::max(1, int(std::lround(step)));
  std::vector<float> smooth(in.size());
  double acc = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    acc += in[i];
    if (i >= size_t(box)) acc -= in[i - size_t(box)];
    smooth[i] = float(acc / std::min<double>(double(i + 1), box));
  }
  const size_t n = size_t(double(in.size()) / step);
  std::vector<float> out(n);
  const double shift = (box - 1) * 0.5;  // the box's delay
  for (size_t i = 0; i < n; ++i) {
    const double x = std::min(double(in.size() - 1), i * step + shift);
    const size_t a = size_t(x);
    const size_t b = std::min(a + 1, in.size() - 1);
    const float t = float(x - double(a));
    out[i] = smooth[a] * (1 - t) + smooth[b] * t;
  }
  return out;
}

void WriteWav(const fs::path& path, const std::vector<float>& pcm) {
  std::ofstream f(path, std::ios::binary);
  auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = uint32_t(pcm.size() * 2);
  f.write("RIFF", 4);
  w32(36 + bytes);
  f.write("WAVEfmt ", 8);
  w32(16);
  w16(1);
  w16(1);
  w32(16000);
  w32(32000);
  w16(2);
  w16(16);
  f.write("data", 4);
  w32(bytes);
  for (float v : pcm) w16(uint16_t(int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
}

// ---- which sounds ------------------------------------------------------------------

bool NamedLike(const std::string& lower, std::initializer_list<const char*> words) {
  for (const char* w : words) {
    if (lower.find(w) != std::string::npos) return true;
  }
  return false;
}

// ---- transcribing --------------------------------------------------------------

struct Work {
  size_t sound = 0;           // index into scan.sounds
  std::vector<float> pcm;     // 16 kHz mono
  uint64_t id = 0;            // cache key: hash of the whole data
  bool video = false;
};

struct Word {
  int64_t t0 = 0, t1 = 0;  // ms, within the clip
  std::string text;
  float p = 0;
};

// Strips what isn't a spoken word: "[Music]", "(laughs)", "*sighs*", notes.
bool IsNonSpeech(const std::string& w) {
  std::string t;
  for (char c : w) {
    if (c != ' ') t += c;
  }
  if (t.empty()) return true;
  if (t.find("\xE2\x99\xAA") != std::string::npos || t.find("\xE2\x99\xAB") != std::string::npos) return true;
  return false;
}

// Words to lines: a new line after a sentence ends, at a pause, or when it
// gets long (two lines of about 42 characters on screen).
std::vector<sf::Line> MakeLines(const std::vector<Word>& words, int64_t clip_ms) {
  std::vector<sf::Line> lines;
  sf::Line cur;
  int64_t last_end = -1;
  auto flush = [&]() {
    std::string t = cur.text;
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (!t.empty()) {
      cur.text = t;
      lines.push_back(cur);
    }
    cur = sf::Line{};
  };
  for (const Word& w : words) {
    const bool first = cur.text.empty();
    if (!first && (w.t0 - last_end > 700 || cur.text.size() + w.text.size() > 84)) flush();
    if (cur.text.empty()) cur.start_ms = uint32_t(std::max<int64_t>(0, w.t0));
    cur.text += w.text;
    cur.end_ms = uint32_t(std::max<int64_t>(w.t1, cur.start_ms));
    last_end = w.t1;
    const char e = w.text.empty() ? 0 : w.text.back();
    if ((e == '.' || e == '?' || e == '!') && cur.text.size() > 24) flush();
  }
  flush();
  // Bracketed noises on a line of their own ("[MUSIC]", "(sighs)") go.
  std::vector<sf::Line> kept;
  for (auto& l : lines) {
    const std::string& t = l.text;
    if ((t.front() == '[' && t.back() == ']') || (t.front() == '(' && t.back() == ')') ||
        (t.front() == '*' && t.back() == '*')) {
      continue;
    }
    kept.push_back(l);
  }
  // On screen long enough to read: at least 1.2 s (or to the next line), and a
  // little after the last word.
  for (size_t i = 0; i < kept.size(); ++i) {
    int64_t end = std::max<int64_t>(kept[i].end_ms + 300, kept[i].start_ms + 1200);
    if (i + 1 < kept.size()) end = std::min<int64_t>(end, kept[i + 1].start_ms);
    end = std::min<int64_t>(end, std::max<int64_t>(clip_ms + 500, kept[i].start_ms + 1200));
    kept[i].end_ms = uint32_t(std::max<int64_t>(end, kept[i].start_ms + 300));
  }
  return kept;
}

std::string Clean(std::string s) {
  for (char& c : s) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return s;
}

struct Transcriber {
  whisper_context* ctx = nullptr;
  int threads = 4;

  // Transcribes `batch` (each ~short) as one window, words given back to
  // the clip they fall in. Returns per-clip words.
  std::vector<std::vector<Word>> Run(const std::vector<const std::vector<float>*>& batch) {
    constexpr int kGap = 16000 * 2;  // 2 s of silence between sounds
    std::vector<float> all;
    std::vector<std::pair<int64_t, int64_t>> span;  // ms
    for (const auto* pcm : batch) {
      if (!all.empty()) all.resize(all.size() + kGap, 0.0f);
      const int64_t a = int64_t(all.size()) / 16;
      all.insert(all.end(), pcm->begin(), pcm->end());
      span.push_back({a, int64_t(all.size()) / 16});
    }
    // whisper wants a bit of room at the end too.
    all.resize(all.size() + 16000 / 2, 0.0f);
    std::vector<std::vector<Word>> out(batch.size());

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    p.n_threads = threads;
    p.language = "en";
    p.translate = false;
    p.no_context = true;
    p.single_segment = false;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_special = false;
    p.print_timestamps = false;
    p.token_timestamps = true;
    p.suppress_blank = true;
    p.suppress_nst = true;
    p.no_speech_thold = 0.6f;
    p.beam_search.beam_size = 5;
    if (whisper_full(ctx, p, all.data(), int(all.size())) != 0) return out;

    const whisper_token eot = whisper_token_eot(ctx);
    const int segments = whisper_full_n_segments(ctx);
    for (int s = 0; s < segments; ++s) {
      if (whisper_full_get_segment_no_speech_prob(ctx, s) > 0.6f) continue;
      const int tokens = whisper_full_n_tokens(ctx, s);
      // Tokens to words (a word starts with a space); a word's time is its
      // tokens' span.
      std::vector<Word> words;
      for (int t = 0; t < tokens; ++t) {
        const whisper_token_data td = whisper_full_get_token_data(ctx, s, t);
        if (td.id >= eot) continue;
        const char* text = whisper_full_get_token_text(ctx, s, t);
        if (!text || !*text) continue;
        const std::string tt = text;
        if (words.empty() || tt[0] == ' ') {
          words.push_back(Word{td.t0 * 10, td.t1 * 10, tt, td.p});
        } else {
          words.back().text += tt;
          words.back().t1 = std::max<int64_t>(words.back().t1, td.t1 * 10);
          words.back().p = std::min(words.back().p, td.p);
        }
      }
      for (Word& w : words) {
        if (IsNonSpeech(w.text)) continue;
        const int64_t mid = (w.t0 + w.t1) / 2;
        // The clip it falls in (or the nearest one).
        size_t best = 0;
        int64_t best_d = INT64_MAX;
        for (size_t c = 0; c < span.size(); ++c) {
          const int64_t d = mid < span[c].first ? span[c].first - mid
                                                : (mid > span[c].second ? mid - span[c].second : 0);
          if (d < best_d) {
            best_d = d;
            best = c;
          }
        }
        if (best_d > 1000) continue;  // in the silence between: a hallucination
        Word local = w;
        local.t0 = std::clamp<int64_t>(w.t0 - span[best].first, 0, span[best].second - span[best].first);
        local.t1 = std::clamp<int64_t>(w.t1 - span[best].first, local.t0,
                                       span[best].second - span[best].first);
        out[best].push_back(local);
      }
    }
    // Clips where whisper only made something up: very unsure throughout.
    for (auto& words : out) {
      if (words.empty()) continue;
      float sum = 0;
      for (const Word& w : words) sum += w.p;
      if (sum / float(words.size()) < 0.35f) words.clear();
    }
    return out;
  }
};

struct Options {
  fs::path game, out, model;
  int threads = 0;
  bool force = false, scan_only = false, include_music = false;
  fs::path dump_wav;
  fs::path test_wav;  // --transcribe-wav: one 16 kHz mono 16-bit WAV, lines printed (checking)
  size_t limit = 0;
};

void Usage() {
  std::printf(
      "amped3-subtitles --game <game folder> --out <subtitles folder> --model <ggml model>\n"
      "    [--threads N] [--force] [--scan] [--dump-wav <folder>] [--include-music] [--limit N]\n");
}

}  // namespace

int main(int argc, char** argv) {
  // whisper.cpp's and FFmpeg's own messages stay quiet (they'd drown the
  // progress lines).
  whisper_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
  av_log_set_level(AV_LOG_QUIET);
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--game") o.game = next();
    else if (a == "--out") o.out = next();
    else if (a == "--model") o.model = next();
    else if (a == "--threads") o.threads = std::stoi(next());
    else if (a == "--force") o.force = true;
    else if (a == "--scan") o.scan_only = true;
    else if (a == "--dump-wav") o.dump_wav = next();
    else if (a == "--include-music") o.include_music = true;
    else if (a == "--limit") o.limit = size_t(std::stoul(next()));
    else if (a == "--transcribe-wav") o.test_wav = next();
    else {
      Usage();
      return 2;
    }
  }
  if (!o.test_wav.empty() && !o.model.empty()) {
    // Checking the speech recognition on its own: a 16 kHz mono WAV, split
    // in two halves transcribed as one batch (as short sounds are).
    Bytes w;
    if (!ReadFile(o.test_wav, &w) || w.size() < 46) return 1;
    std::vector<float> pcm;
    for (size_t i = 44; i + 1 < w.size(); i += 2) pcm.push_back(float(int16_t(Le16(&w[i]))) / 32768.0f);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    Transcriber tr;
    tr.threads = o.threads > 0 ? o.threads : 4;
    tr.ctx = whisper_init_from_file_with_params(o.model.string().c_str(), cp);
    if (!tr.ctx) return 1;
    const size_t half = pcm.size() / 2;
    std::vector<float> a(pcm.begin(), pcm.begin() + std::ptrdiff_t(half)), b(pcm.begin() + std::ptrdiff_t(half), pcm.end());
    const auto words = tr.Run({&a, &b});
    for (size_t k = 0; k < words.size(); ++k) {
      for (const auto& l : MakeLines(words[k], int64_t((k ? b : a).size() / 16))) {
        std::printf("part %zu  %6u %6u  %s\n", k, l.start_ms, l.end_ms, l.text.c_str());
      }
    }
    whisper_free(tr.ctx);
    return 0;
  }
  if (o.game.empty() || o.out.empty() || (o.model.empty() && !o.scan_only)) {
    Usage();
    return 2;
  }
  std::error_code ec;
  fs::create_directories(o.out / "work", ec);
  if (!o.dump_wav.empty()) fs::create_directories(o.dump_wav, ec);
  if (o.threads <= 0) o.threads = int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));

  // ---- find the sounds
  std::printf("Looking for the game's sounds in %s\n", o.game.string().c_str());
  Scan scan;
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(o.game, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& f : files) {
    const std::string rel = fs::relative(f, o.game, ec).generic_string();
    const std::string ext = Lower(f.extension().string());
    if (ext == ".xex") continue;
    const auto size = fs::file_size(f, ec);
    if (ec || size > (uint64_t(3) << 30)) continue;
    Bytes d;
    if (!ReadFile(f, &d)) continue;
    FromData(rel, ext, d, &scan);
  }

  // ---- decide which to transcribe
  // Banks that are mostly long stereo sounds hold the music.
  std::map<std::string, std::pair<int, int>> bank_stats;  // container -> (long stereo, all)
  struct Pick {
    size_t index;
    bool video;
    std::string why;
  };
  std::vector<Pick> picks;
  std::vector<std::string> decisions;
  // Rough length from the data size (XMA runs ~ 8:1 to 16:1 against 16-bit
  // PCM); the real length comes from decoding.
  auto rough_seconds = [](const Sound& s) {
    return double(s.data.size()) * 10.0 / (2.0 * std::max(1, s.rate) * std::max(1, s.channels));
  };
  for (const Sound& s : scan.sounds) {
    if (!s.in_bank) continue;
    auto& st = bank_stats[s.container];
    st.second++;
    if (s.channels >= 2 && rough_seconds(s) > 60) st.first++;
  }
  for (size_t i = 0; i < scan.sounds.size(); ++i) {
    const Sound& s = scan.sounds[i];
    const std::string lower = Lower(s.source + " " + s.bank);
    std::string leaf = s.container.substr(s.container.find_last_of("/\\:") + 1);
    const std::string stem = Lower(fs::path(leaf).stem().string());
    const bool video = scan.video_stems.count(stem) != 0 ||
                       NamedLike(lower, {"movie", "video", "fmv", "bink", "intro", "credits", "trailer"});
    const bool speechy = video || NamedLike(lower, {"scene", "cinem", "cine", "dialog", "voice", "vo_",
                                                     "/vo", "speech", "story", "talk", "chatter"});
    const double secs = rough_seconds(s);
    std::string why;
    bool take = true;
    if (s.data.size() < 4096) {
      take = false;
      why = "too short";
    } else if (!o.include_music && !speechy) {
      const auto it = bank_stats.find(s.container);
      const bool music_bank = it != bank_stats.end() && it->second.second >= 4 &&
                              it->second.first * 2 >= it->second.second;
      if (NamedLike(lower, {"music", "song", "soundtrack", "radio"})) {
        take = false;
        why = "named like music";
      } else if (music_bank) {
        take = false;
        why = "in a bank of long stereo sounds (music)";
      } else if (s.channels >= 2 && secs > 150) {
        take = false;
        why = "long stereo sound (music)";
      }
    }
    char line[512];
    std::snprintf(line, sizeof(line), "  %-5s %-7s %dch %5d Hz ~%6.1f s  %s%s%s", take ? "yes" : "no",
                  video ? "video" : "speech", s.channels, s.rate, secs, s.source.c_str(),
                  why.empty() ? "" : "  -- ", why.c_str());
    decisions.push_back(line);
    if (take) picks.push_back({i, video, why});
  }

  {
    std::ofstream rep(o.out / "scan.txt");
    rep << "amped3-subtitles: what was found in " << o.game.string() << "\n\n";
    for (const auto& l : scan.report) rep << l << "\n";
    rep << "\nOther files (by extension: count, first bytes of one):\n";
    for (const auto& [k, n] : scan.other_kinds) rep << "  " << k << ": " << n << "  " << scan.other_magic[k] << "\n";
    rep << "\nSounds (" << scan.sounds.size() << "; transcribed: yes/no):\n";
    for (const auto& l : decisions) rep << l << "\n";
  }
  std::printf("Found %zu XMA sounds (%zu to transcribe) and %zu movies; list in %s\n", scan.sounds.size(),
              picks.size(), scan.video_stems.size(), (o.out / "scan.txt").string().c_str());
  if (o.scan_only) return 0;
  if (picks.empty()) {
    std::printf("Nothing to transcribe.\n");
    return 1;
  }
  if (o.limit && picks.size() > o.limit) picks.resize(o.limit);

  // ---- earlier progress
  const fs::path cache_path = o.out / "work" / "transcripts.cache";
  const std::string model_tag = Lower(o.model.filename().string());
  std::unordered_map<uint64_t, std::pair<uint32_t, std::vector<sf::Line>>> cache;  // id -> (ms, lines)
  if (!o.force) {
    std::ifstream f(cache_path);
    std::string line;
    uint64_t id = 0;
    bool ok = false;
    std::pair<uint32_t, std::vector<sf::Line>>* cur = nullptr;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.compare(0, 6, "model ") == 0) {
        ok = line.substr(6) == model_tag;
        continue;
      }
      if (!ok) continue;
      unsigned long long h = 0;
      unsigned ms = 0;
      if (line.compare(0, 6, "sound ") == 0 && std::sscanf(line.c_str() + 6, "%llx %u", &h, &ms) == 2) {
        id = h;
        cur = &cache[id];
        cur->first = ms;
        cur->second.clear();
        continue;
      }
      if (!cur) continue;
      unsigned a = 0, b = 0;
      int used = 0;
      if (std::sscanf(line.c_str(), "%u %u %n", &a, &b, &used) >= 2 && used > 0) {
        cur->second.push_back(sf::Line{a, b, line.substr(used)});
      }
    }
  }
  std::ofstream cache_out(cache_path, std::ios::app);
  cache_out << "model " << model_tag << "\n";

  // ---- the model
  whisper_context_params cp = whisper_context_default_params();
  cp.use_gpu = false;
  Transcriber tr;
  tr.threads = o.threads;
  tr.ctx = whisper_init_from_file_with_params(o.model.string().c_str(), cp);
  if (!tr.ctx) {
    std::fprintf(stderr, "Couldn't load the speech recognition model %s\n", o.model.string().c_str());
    return 1;
  }

  // ---- decode, batch, transcribe
  std::vector<sf::Clip> clips(picks.size());
  std::vector<bool> have(picks.size(), false);
  const double t_start = Now();
  size_t done = 0, from_cache = 0, failed = 0;
  double audio_secs = 0;
  std::vector<size_t> pending;  // indices into picks, decoded, waiting for a batch
  std::vector<std::vector<float>> pending_pcm;
  size_t pending_samples = 0;
  std::vector<uint64_t> ids(picks.size());

  auto finish = [&](size_t k, uint32_t ms, std::vector<sf::Line> lines) {
    sf::Clip& c = clips[k];
    const Sound& s = scan.sounds[picks[k].index];
    c.video = picks[k].video;
    c.duration_ms = ms;
    c.bytes = uint32_t(s.data.size());
    c.keys = sf::KeysOf(s.data.data(), s.data.size());
    c.source = s.source;
    c.lines = std::move(lines);
    have[k] = true;
    ++done;
  };
  auto run_batch = [&]() {
    if (pending.empty()) return;
    std::vector<const std::vector<float>*> batch;
    for (const auto& pcm : pending_pcm) batch.push_back(&pcm);
    const auto words = tr.Run(batch);
    for (size_t b = 0; b < pending.size(); ++b) {
      const size_t k = pending[b];
      const uint32_t ms = uint32_t(pending_pcm[b].size() / 16);
      auto lines = MakeLines(words[b], ms);
      cache_out << "sound " << std::hex << ids[k] << std::dec << " " << ms << "\n";
      for (const auto& l : lines) cache_out << l.start_ms << " " << l.end_ms << " " << Clean(l.text) << "\n";
      finish(k, ms, std::move(lines));
    }
    cache_out.flush();
    pending.clear();
    pending_pcm.clear();
    pending_samples = 0;
  };
  constexpr size_t kBatchSamples = 16000 * 24;
  double last_print = 0;
  for (size_t k = 0; k < picks.size(); ++k) {
    const Sound& s = scan.sounds[picks[k].index];
    ids[k] = sf::Fnv1a64(s.data.data(), s.data.size()) ^ (uint64_t(s.channels) << 56) ^ uint64_t(s.rate);
    if (auto it = cache.find(ids[k]); it != cache.end()) {
      finish(k, it->second.first, it->second.second);
      ++from_cache;
      continue;
    }
    std::vector<float> native;
    if (!DecodeXma(s, &native)) {
      ++failed;
      continue;
    }
    std::vector<float> pcm = To16k(native, s.rate);
    float peak = 0;
    for (float v : pcm) peak = std::max(peak, std::fabs(v));
    audio_secs += double(pcm.size()) / 16000.0;
    if (!o.dump_wav.empty()) {
      std::string name;
      for (char c : s.source) name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
      WriteWav(o.dump_wav / (name + ".wav"), pcm);
    }
    if (peak < 0.003f || pcm.size() < 16000 / 4) {
      // Silent, or too short for a word.
      const uint32_t ms = uint32_t(pcm.size() / 16);
      cache_out << "sound " << std::hex << ids[k] << std::dec << " " << ms << "\n";
      finish(k, ms, {});
      continue;
    }
    const float gain = std::min(0.9f / peak, 30.0f);
    for (float& v : pcm) v *= gain;
    if (pcm.size() > kBatchSamples) {
      // Long: on its own (whisper goes through it 30 s at a time).
      run_batch();
      pending.push_back(k);
      pending_pcm.push_back(std::move(pcm));
      run_batch();
    } else {
      if (pending_samples + pcm.size() + 32000 > kBatchSamples) run_batch();
      pending_samples += pcm.size() + 32000;
      pending.push_back(k);
      pending_pcm.push_back(std::move(pcm));
    }
    const double now = Now();
    if (now - last_print > 5.0) {
      last_print = now;
      std::printf("  %zu / %zu sounds (%.0f min of audio, %.0f s so far)\n", done, picks.size(),
                  audio_secs / 60.0, now - t_start);
      std::fflush(stdout);
    }
  }
  run_batch();
  whisper_free(tr.ctx);

  // ---- the file
  // Keys shared by clips that say different things can't tell them apart:
  // dropped (a longer key of theirs still can).
  std::map<std::pair<uint32_t, uint64_t>, std::set<std::string>> seen;
  auto said = [](const sf::Clip& c) {
    std::string s;
    for (const auto& l : c.lines) s += l.text + "|";
    return s;
  };
  for (size_t k = 0; k < clips.size(); ++k) {
    if (!have[k]) continue;
    for (const auto& key : clips[k].keys) seen[{key.length, key.hash}].insert(said(clips[k]));
  }
  std::ostringstream text;
  text << sf::kMagic << "\n";
  text << "# Made on this PC by amped3-subtitles from this copy of the game (model " << model_tag << ").\n";
  text << "# Lines: clip <kind> <ms> <bytes> <keys> <source> / <start ms> <end ms> <text>\n";
  size_t with_text = 0, lines_total = 0;
  for (size_t k = 0; k < clips.size(); ++k) {
    if (!have[k] || clips[k].lines.empty()) continue;
    std::vector<sf::Key> keys;
    for (const auto& key : clips[k].keys) {
      if (seen[{key.length, key.hash}].size() == 1) keys.push_back(key);
    }
    if (keys.empty()) continue;
    const sf::Clip& c = clips[k];
    text << "clip " << (c.video ? "video" : "speech") << " " << c.duration_ms << " " << c.bytes << " "
         << sf::FormatKeys(keys) << " " << Clean(c.source) << "\n";
    for (const auto& l : c.lines) text << l.start_ms << " " << l.end_ms << " " << Clean(l.text) << "\n";
    ++with_text;
    lines_total += c.lines.size();
  }
  const fs::path out_file = o.out / sf::kFileName;
  const fs::path tmp = o.out / (std::string(sf::kFileName) + ".new");
  {
    std::ofstream f(tmp, std::ios::binary);
    f << text.str();
    if (!f) {
      std::fprintf(stderr, "Couldn't write %s\n", tmp.string().c_str());
      return 1;
    }
  }
  fs::rename(tmp, out_file, ec);
  if (ec) {
    fs::copy_file(tmp, out_file, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
  }
  std::printf("Done: %zu sounds with speech (%zu subtitle lines) of %zu transcribed (%zu from earlier runs, "
              "%zu couldn't be decoded), in %.0f s.\nWrote %s\n",
              with_text, lines_total, done, from_cache, failed, Now() - t_start, out_file.string().c_str());
  return 0;
}
