// amped3 - camera scan (F8, diagnostics): where the game keeps its camera.
//
// For the noclip freecam (the game itself drawing from the freecam, so the
// whole world is there to explore), the port has to set the game's camera,
// which means knowing where it lives. The native renderer knows the camera
// exactly each frame (the lighting pass's position and view); this scans the
// game's committed memory for that position (three big-endian floats) and its
// forward and right vectors, and writes every place found, with the values
// around it, to logs\amped3_camera_scan_<time>.txt. Best with the game paused
// (the memory then holds what the frame was drawn with); two scans from
// different spots tell the camera from copies that only happened to match.

#include "camera_scan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <rex/kernel.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "live_renderer.h"
#include "notice.h"

namespace amped3::camera_scan {
namespace {

std::filesystem::path g_dir;
std::atomic<bool> g_busy{false};

rex::memory::Memory* GuestMemory() {
  auto* kernel = rex::system::kernel_state();
  return kernel ? kernel->memory() : nullptr;
}

float F(const uint8_t* p) {
  const uint32_t b = rex::memory::load_and_swap<uint32_t>(p);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

struct Hit {
  uint32_t address;
  const char* what;
  float sign;  // the vector as found (1) or negated (-1)
};

void ScanProperties(const char* stamp);
void Scan(const float cam[9]) {
  auto* memory = GuestMemory();
  if (!memory) return;
  std::vector<Hit> hits;
  // What to look for: the position, and each axis either way round.
  struct Target {
    const char* what;
    float v[3];
    float tol;
    float sign;
  };
  std::vector<Target> targets = {{"position", {cam[0], cam[1], cam[2]}, 0.02f, 1.0f}};
  const char* names[2] = {"forward", "right"};
  for (int a = 0; a < 2; ++a) {
    for (float sign : {1.0f, -1.0f}) {
      targets.push_back({names[a],
                         {sign * cam[3 + a * 3], sign * cam[4 + a * 3], sign * cam[5 + a * 3]},
                         0.003f,
                         sign});
    }
  }
  uint64_t scanned = 0;
  // Guest virtual memory below the physical mirrors (0xC0000000 and up show
  // the same memory as 0xA0000000 again).
  uint32_t address = 0x10000;
  while (address < 0xC0000000u && hits.size() < 4000) {
    auto* heap = memory->LookupHeap(address);
    if (!heap) {
      address = (address + 0x10000) & ~0xFFFFu;
      if (!address) break;
      continue;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(address, &info) || !info.region_size) {
      address = (address + heap->page_size()) & ~(heap->page_size() - 1);
      continue;
    }
    const uint32_t start = info.base_address, size = info.region_size;
    const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
                           heap->QueryRangeAccess(start, start + size - 1) !=
                               rex::memory::PageAccess::kNoAccess;
    if (committed && size >= 12) {
      const uint8_t* p = memory->TranslateVirtual(start);
      scanned += size;
      for (uint32_t off = 0; off + 12 <= size && hits.size() < 4000; off += 4) {
        const float x = F(p + off);
        if (!std::isfinite(x)) continue;
        for (const Target& t : targets) {
          // (Written as "not within": a NaN is never within.)
          if (!(std::fabs(x - t.v[0]) <= t.tol)) continue;
          if (!(std::fabs(F(p + off + 4) - t.v[1]) <= t.tol)) continue;
          if (!(std::fabs(F(p + off + 8) - t.v[2]) <= t.tol)) continue;
          hits.push_back({start + off, t.what, t.sign});
        }
      }
    }
    const uint32_t next = start + size;
    if (next <= address) break;
    address = next;
  }
  // The report.
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  const auto path = g_dir / (std::string("amped3_camera_scan_") + stamp + ".txt");
  std::ofstream out(path);
  char line[512];
  std::snprintf(line, sizeof(line),
                "# Amped 3 camera scan %s\n# position %.4f %.4f %.4f  forward %.4f %.4f %.4f  "
                "right %.4f %.4f %.4f\n# game state %d, scene still %d, %.0f MB scanned, %zu "
                "places\n",
                stamp, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8],
                live::GameState(), int(live::SceneStill()), double(scanned) / 1048576.0,
                hits.size());
  out << line;
  for (const Hit& h : hits) {
    std::snprintf(line, sizeof(line), "\n%08X %s%s\n", h.address, h.sign < 0 ? "-" : "", h.what);
    out << line;
    // 16 floats before and 32 after, four to a line (with their addresses).
    const uint32_t from = h.address >= 64 ? h.address - 64 : 0;
    for (uint32_t a = from; a < h.address + 128; a += 16) {
      auto* heap = memory->LookupHeap(a);
      if (!heap || heap->QueryRangeAccess(a, a + 15) == rex::memory::PageAccess::kNoAccess) {
        continue;
      }
      const uint8_t* p = memory->TranslateVirtual(a);
      std::snprintf(line, sizeof(line), "  %08X: %12.5g %12.5g %12.5g %12.5g   %08X %08X %08X %08X\n",
                    a, F(p), F(p + 4), F(p + 8), F(p + 12),
                    rex::memory::load_and_swap<uint32_t>(p),
                    rex::memory::load_and_swap<uint32_t>(p + 4),
                    rex::memory::load_and_swap<uint32_t>(p + 8),
                    rex::memory::load_and_swap<uint32_t>(p + 12));
      out << line;
    }
  }
  REXLOG_INFO("Camera scan: {} places ({} MB scanned) written to {}", hits.size(),
              scanned >> 20, path.string());
  ScanProperties(stamp);
  notice::Show("Camera scan: " + std::to_string(hits.size()) + " places written to logs");
}

// Property scan (with the camera scan, F8): where the game keeps the names of
// its riding tunables (the jump zone, the ground's hold, the spins' revert),
// and what points at each name (a reflection table entry: name, type, member
// offset), for finding the code that reads them. To
// logs\amped3_property_scan_<time>.txt.
void ScanProperties(const char* stamp) {
  auto* memory = GuestMemory();
  if (!memory) return;
  static const char* kNames[] = {
      "MinSpeedSquaredForJumpZone", "MaxSpeedForNonJumpZone", "MaxSpeedForPerfectJump",
      "MaxSpeedForHorribleJump", "PerfectJumpTime", "HorribleJumpTime", "PerfectHangTimeModifier",
      "m_noJumpTime", "JumpZone", "StickyGround", "MinGroundDistanceOnGround",
      "MinGroundDistanceInAir", "StickyDistance", "MinDownwardVelocity", "EnterAirVelocityScalar",
      "AdditionalNoJumpVelocityScalar", "JumpSpeedRange", "JumpSpeedScalarRange", "MaxJumpHeight",
      "RevertRotationSlerpSpeed", "RotationAmountStyleThreshold", "CrashThreshold",
      "SpinAirAngularDampening", "TimeToBeginLandPose", "LandingPlanning", "XLerpAlpha",
      "InAirBlendAwayAlpha", "MaxTimePerIntegrationStep"};
  struct Region {
    uint32_t start, size;
  };
  std::vector<Region> regions;
  uint32_t address = 0x10000;
  while (address < 0xC0000000u) {
    auto* heap = memory->LookupHeap(address);
    if (!heap) {
      address = (address + 0x10000) & ~0xFFFFu;
      if (!address) break;
      continue;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(address, &info) || !info.region_size) {
      address = (address + heap->page_size()) & ~(heap->page_size() - 1);
      continue;
    }
    const uint32_t start = info.base_address, size = info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) != 0 && size >= 16 &&
        heap->QueryRangeAccess(start, start + size - 1) != rex::memory::PageAccess::kNoAccess) {
      regions.push_back({start, size});
    }
    const uint32_t next = start + size;
    if (next <= address) break;
    address = next;
  }
  const auto path = g_dir / (std::string("amped3_property_scan_") + stamp + ".txt");
  std::ofstream out(path);
  out << "# Amped 3 property scan " << stamp << "\n";
  char line[512];
  auto dump = [&](uint32_t at) {
    const uint32_t from = at >= 32 ? at - 32 : 0;
    for (uint32_t a = from; a < at + 48; a += 16) {
      auto* heap = memory->LookupHeap(a);
      if (!heap || heap->QueryRangeAccess(a, a + 15) == rex::memory::PageAccess::kNoAccess) continue;
      const uint8_t* q = memory->TranslateVirtual(a);
      std::snprintf(line, sizeof(line), "    %08X: %08X %08X %08X %08X  %12.5g %12.5g %12.5g %12.5g\n", a,
                    rex::memory::load_and_swap<uint32_t>(q), rex::memory::load_and_swap<uint32_t>(q + 4),
                    rex::memory::load_and_swap<uint32_t>(q + 8), rex::memory::load_and_swap<uint32_t>(q + 12),
                    F(q), F(q + 4), F(q + 8), F(q + 12));
      out << line;
    }
  };
  // The names, then (one pass) what points at each.
  std::vector<std::pair<const char*, uint32_t>> strings;
  for (const char* name : kNames) {
    const size_t n = std::strlen(name);
    size_t found = 0;
    for (const Region& r : regions) {
      const uint8_t* p = memory->TranslateVirtual(r.start);
      const uint8_t* end = p + r.size;
      for (const uint8_t* q = p; q + n + 1 <= end && found < 64;) {
        q = static_cast<const uint8_t*>(std::memchr(q, name[0], size_t(end - q)));
        if (!q || q + n + 1 > end) break;
        // (Whole: a terminating zero, and not the tail of a longer name.)
        if (std::memcmp(q, name, n) == 0 && q[n] == 0 && (q == p || q[-1] < 'A')) {
          strings.push_back({name, r.start + uint32_t(q - p)});
          ++found;
        }
        ++q;
      }
    }
  }
  std::map<uint32_t, std::vector<uint32_t>> refs;
  for (const auto& st : strings) refs[st.second];
  for (const Region& r : regions) {
    const uint8_t* p = memory->TranslateVirtual(r.start);
    for (uint32_t off = 0; off + 4 <= r.size; off += 4) {
      const uint32_t v = rex::memory::load_and_swap<uint32_t>(p + off);
      if (v < 0x10000) continue;
      auto it = refs.find(v);
      if (it != refs.end() && it->second.size() < 16) it->second.push_back(r.start + off);
    }
  }
  const char* last = nullptr;
  for (const auto& [name, at] : strings) {
    if (name != last) {
      out << "\n" << name << "\n";
      last = name;
    }
    std::snprintf(line, sizeof(line), "  string at %08X\n", at);
    out << line;
    for (uint32_t ptr : refs[at]) {
      std::snprintf(line, sizeof(line), "   pointer at %08X\n", ptr);
      out << line;
      dump(ptr);
    }
  }
  const size_t total = strings.size();
  REXLOG_INFO("Property scan: {} names found, written to {}", total, path.string());
}

}  // namespace

void Install(const std::filesystem::path& log_dir) { g_dir = log_dir; }

void Run() {
  float cam[9];
  if (!live::GameCamera(cam)) {
    notice::Show("Camera scan: no 3D scene seen yet (native renderer)");
    return;
  }
  if (g_busy.exchange(true)) return;
  std::thread([c = std::vector<float>(cam, cam + 9)] {
    Scan(c.data());
    g_busy = false;
  }).detach();
}

}  // namespace amped3::camera_scan
