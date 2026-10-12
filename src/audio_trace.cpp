// amped3 - audio diagnostics (see audio_trace.h).
//
// Guest side (mid-asm hooks in amped3_functions.toml, each placed after the
// "mflr r12" that opens the function, so r12 is the caller's return address):
//
//   0x82414530  queue a cue play request    (r4 = 232-byte request:
//               +76 sound object, +84 cue handle, +96 cue name, +158 flag)
//   0x82414F58  queue a cue stop/command    (r4: +4 name, +8 -> handle, +12
//               handle, +16 flag)
//   0x824146F8  queue a category pause / resume (r4: +4 category name,
//               +12 1 = pause, 2 = resume)
//   0x82414978  queue a category volume change (r4: +4 name, floats after)
//   0x82414B00  queue another category command (r4: dumped)
//
// Host side: a thread samples the 320 XMA contexts every 50 ms (straight from
// guest memory, as the decoder sees them) and logs the streams that keep
// decoding for more than 3 seconds - in practice the music - when they start
// and stop, plus a summary every 10 seconds.

#include "audio_trace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/audio/audio_system.h>
#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(trace_audio, false, "Amped 3/Diagnostics",
                    "Log the game's music and sound requests and which audio streams are "
                    "playing (to the log file). For finding the overlapping music bug.");

namespace {

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

// A host pointer to `size` readable bytes at guest virtual `address`, or null.
const uint8_t* Readable(rex::memory::Memory* memory, uint32_t address, uint32_t size) {
  if (!memory || address < 0x10000 || address > 0xFFFF0000u - size) return nullptr;
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  size_t length = size;
  rex::memory::PageAccess access;
  if (!rex::memory::QueryProtect(host, length, access)) return nullptr;
  if (access == rex::memory::PageAccess::kNoAccess) return nullptr;
  return host;
}

uint32_t U32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Guest word at `address`, or 0 when it isn't readable.
uint32_t GuestU32(rex::memory::Memory* memory, uint32_t address) {
  const uint8_t* p = Readable(memory, address, 4);
  return p ? U32(p) : 0;
}

// The C string at guest `address` if it looks like one (2..80 printable
// characters), else empty.
std::string GuestString(rex::memory::Memory* memory, uint32_t address) {
  const uint8_t* p = Readable(memory, address, 1);
  if (!p) return {};
  std::string s;
  for (uint32_t i = 0; i < 80; ++i) {
    // Re-check across a page boundary.
    if (((address + i) & 0xFFF) == 0 && i && !Readable(memory, address + i, 1)) return {};
    const uint8_t c = p[i];
    if (c == 0) return s.size() >= 2 ? s : std::string();
    if (c < 0x20 || c > 0x7E) return {};
    s.push_back(char(c));
  }
  return {};
}

// "w0 w1 ..." for `count` words of the struct at `address`; words that point
// at a string are shown as the string, words that look like floats in a
// plausible range also as the float.
std::string DumpWords(rex::memory::Memory* memory, uint32_t address, uint32_t count) {
  std::string out;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t w = GuestU32(memory, address + 4 * i);
    if (i) out += ' ';
    out += fmt::format("+{}={:08X}", 4 * i, w);
    const std::string s = w ? GuestString(memory, w) : std::string();
    if (!s.empty()) {
      out += fmt::format("('{}')", s);
    } else {
      float f;
      std::memcpy(&f, &w, 4);
      const uint32_t exponent = (w >> 23) & 0xFF;
      if (w && exponent >= 100 && exponent <= 140) out += fmt::format("({:g})", f);
    }
  }
  return out;
}

// Log `line` for hook `slot`, folding identical consecutive lines.
void Emit(int slot, const std::string& line) {
  static std::mutex mutex;
  static std::array<std::string, 8> last;
  static std::array<uint32_t, 8> repeats{};
  std::lock_guard lock(mutex);
  if (last[slot] == line) {
    ++repeats[slot];
    return;
  }
  if (repeats[slot]) {
    REXLOG_INFO("audio-trace: (previous {} request repeated {} more times)", slot,
                repeats[slot]);
  }
  repeats[slot] = 0;
  last[slot] = line;
  REXLOG_INFO("audio-trace: {}", line);
}

bool Enabled() { return REXCVAR_GET(trace_audio); }

}  // namespace

void Amped3_TraceAudioPlay(PPCRegister& r12, PPCRegister& r4) {
  if (!Enabled()) return;
  auto* memory = GuestMemory();
  const uint32_t req = r4.u32;
  const uint32_t name_ptr = GuestU32(memory, req + 96);
  const uint8_t* flag = Readable(memory, req + 158, 1);
  Emit(0, fmt::format("PLAY  from {:08X}: cue '{}' handle {:08X} sound {:08X} flag158 {} | {}",
                      r12.u32, GuestString(memory, name_ptr), GuestU32(memory, req + 84),
                      GuestU32(memory, req + 76), flag ? int(*flag) : -1,
                      DumpWords(memory, req + 160, 8)));
}

void Amped3_TraceAudioCommand(PPCRegister& r12, PPCRegister& r4) {
  if (!Enabled()) return;
  auto* memory = GuestMemory();
  const uint32_t cmd = r4.u32;
  const uint32_t ptr = GuestU32(memory, cmd + 8);
  Emit(1, fmt::format("CMD   from {:08X}: {} | handle via +8: {:08X}", r12.u32,
                      DumpWords(memory, cmd, 6), ptr ? GuestU32(memory, ptr) : 0));
}

void Amped3_TraceAudioPause(PPCRegister& r12, PPCRegister& r4) {
  if (!Enabled()) return;
  auto* memory = GuestMemory();
  Emit(2, fmt::format("PAUSE from {:08X}: {}", r12.u32, DumpWords(memory, r4.u32, 5)));
}

void Amped3_TraceAudioVolume(PPCRegister& r12, PPCRegister& r4) {
  if (!Enabled()) return;
  auto* memory = GuestMemory();
  Emit(3, fmt::format("VOL   from {:08X}: {}", r12.u32, DumpWords(memory, r4.u32, 8)));
}

void Amped3_TraceAudioCategory(PPCRegister& r12, PPCRegister& r4) {
  if (!Enabled()) return;
  auto* memory = GuestMemory();
  Emit(4, fmt::format("CAT   from {:08X}: {}", r12.u32, DumpWords(memory, r4.u32, 10)));
}

namespace amped3::audio_trace {
namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};

struct Stream {
  bool seen = false;          // context initialized (has an output buffer)
  uint32_t words[10] = {};    // last sample of dwords 0..9
  double last_change = 0;     // seconds
  double start = -1;          // when this run of activity began (-1: idle)
  bool reported = false;      // start logged (ran > 3 s)
  uint64_t changes = 0;
};

std::string Describe(uint32_t index, const rex::audio::XMA_CONTEXT_DATA& d) {
  static const int kRates[4] = {24000, 32000, 44100, 48000};
  return fmt::format("ctx {} ({} Hz {}, in0 {:08X}x{} in1 {:08X}x{}, out {:08X}, loop {})", index,
                     kRates[d.sample_rate & 3], d.is_stereo ? "stereo" : "mono",
                     uint32_t(d.input_buffer_0_ptr), uint32_t(d.input_buffer_0_packet_count),
                     uint32_t(d.input_buffer_1_ptr), uint32_t(d.input_buffer_1_packet_count),
                     uint32_t(d.output_buffer_ptr), uint32_t(d.loop_count));
}

void Monitor(rex::Runtime* runtime) {
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  std::array<Stream, 320> streams{};
  double next_summary = 10;
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!Enabled()) continue;
    auto* audio = static_cast<rex::audio::AudioSystem*>(runtime->audio_system());
    auto* decoder = audio ? audio->xma_decoder() : nullptr;
    auto* memory = runtime->memory();
    if (!decoder || !memory || !decoder->context_array_ptr()) continue;
    const double now = std::chrono::duration<double>(Clock::now() - t0).count();
    const uint8_t* base = memory->TranslatePhysical<uint8_t*>(decoder->context_array_ptr());
    for (uint32_t i = 0; i < streams.size(); ++i) {
      const uint8_t* raw = base + 64 * i;
      Stream& s = streams[i];
      uint32_t w[10];
      for (int k = 0; k < 10; ++k) w[k] = U32(raw + 4 * k);
      const bool initialized = w[7] != 0;  // output buffer pointer
      if (!initialized) {
        if (s.reported) {
          REXLOG_INFO("audio-trace: XMA ctx {} released after {:.1f} s of decoding", i,
                      s.last_change - s.start);
        }
        s = Stream{};
        continue;
      }
      // Decoding moves the read / write offsets and swaps input buffers.
      const bool changed = !s.seen || std::memcmp(w, s.words, sizeof(w)) != 0;
      std::memcpy(s.words, w, sizeof(w));
      s.seen = true;
      if (changed) {
        ++s.changes;
        if (s.start < 0) s.start = now;
        s.last_change = now;
      }
      const rex::audio::XMA_CONTEXT_DATA d(raw);
      if (s.start >= 0 && !s.reported && now - s.start > 3.0) {
        s.reported = true;
        REXLOG_INFO("audio-trace: XMA stream playing for 3 s: {}", Describe(i, d));
      }
      if (s.start >= 0 && now - s.last_change > 1.0) {
        if (s.reported) {
          REXLOG_INFO("audio-trace: XMA stream stopped after {:.1f} s: {}",
                      s.last_change - s.start, Describe(i, d));
        }
        s.start = -1;
        s.reported = false;
      }
    }
    if (now >= next_summary) {
      next_summary = now + 10;
      std::string list;
      int count = 0;
      for (uint32_t i = 0; i < streams.size(); ++i) {
        const Stream& s = streams[i];
        if (s.start >= 0 && s.reported) {
          list += fmt::format(" [ctx {} {:.0f} s]", i, now - s.start);
          ++count;
        }
      }
      REXLOG_INFO("audio-trace: {} long-running XMA streams:{}", count, list);
    }
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  if (!runtime || g_running.exchange(true)) return;
  g_thread = std::thread(Monitor, runtime);
}

void Stop() {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace amped3::audio_trace
