// amped3 - performance overlay. See perf_overlay.h.

#include "perf_overlay.h"
#include "perf_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace amped3::perf {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kBind = "bind_performance_overlay";

// --- Per-frame times -----------------------------------------------------------

struct FrameSample {
  float frame = 0;   // swap to swap (ms)
  float cpu = 0;     // game thread CPU time (ms)
  float render = -1; // native renderer worker (ms; < 0 none)
  float gpu = -1;    // GPU (ms; < 0 none)
};
constexpr size_t kHistory = 360;

std::mutex g_frames_mutex;
std::array<FrameSample, kHistory> g_frames{};
size_t g_frames_next = 0;   // next slot to write
size_t g_frames_count = 0;  // filled slots

// Game thread state (only touched on the game thread).
Clock::time_point g_last_begin{};
uint64_t g_end_cycles = 0;
bool g_have_end = false;

// Thread cycles to milliseconds: QueryThreadCycleTime counts in TSC ticks;
// their rate is measured against the steady clock since the first swap.
uint64_t g_tsc_start = 0;
Clock::time_point g_tsc_start_time{};
std::atomic<double> g_tsc_per_ms{0.0};

// Latest worker / GPU reports and when they came.
std::atomic<double> g_render_ms{-1.0}, g_gpu_ms{-1.0};
std::atomic<int64_t> g_render_at{0}, g_gpu_at{0};  // steady clock, ns
std::atomic<uint64_t> g_presented{0};  // native renderer frames shown

std::mutex g_gpu_name_mutex;
std::string g_gpu_name;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

uint64_t ThreadCycles() {
#if defined(_WIN32)
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
#else
  // The thread's own CPU time (waits don't count), like the cycle count on
  // Windows. Used to be the wall clock, which counted the waits too.
  return uint64_t(amped3::trace::ThreadCpuNs());
#endif
}

uint64_t Tsc() {
#if defined(_WIN32)
  return __rdtsc();
#else
  return uint64_t(NowNs());
#endif
}

// --- System counters -------------------------------------------------------------

struct SystemSample {
  float cpu_pct = -1, proc_pct = -1, cpu_mhz = -1, cpu_w = -1;
  float gpu_pct = -1, gpu_mhz = -1, gpu_w = -1;
  std::string gpu_source;  // where the GPU clock / power come from
};
std::mutex g_sys_mutex;
SystemSample g_sys;

#if defined(_WIN32)

// NVIDIA: NVML (nvml.dll, installed with the driver).
struct Nvml {
  HMODULE dll = nullptr;
  void* device = nullptr;
  using InitFn = int (*)();
  using ShutdownFn = int (*)();
  using CountFn = int (*)(unsigned*);
  using HandleFn = int (*)(unsigned, void**);
  using ClockFn = int (*)(void*, int, unsigned*);
  using PowerFn = int (*)(void*, unsigned*);
  ShutdownFn shutdown = nullptr;
  ClockFn clock = nullptr;
  PowerFn power = nullptr;

  bool Open() {
    dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
      // Older drivers: under Program Files (wherever that is).
      wchar_t pf[MAX_PATH];
      const DWORD n = GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH);
      if (n > 0 && n < MAX_PATH) {
        dll = LoadLibraryW((std::wstring(pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll").c_str());
      }
    }
    if (!dll) return false;
    auto init = reinterpret_cast<InitFn>(GetProcAddress(dll, "nvmlInit_v2"));
    auto count = reinterpret_cast<CountFn>(GetProcAddress(dll, "nvmlDeviceGetCount_v2"));
    auto handle = reinterpret_cast<HandleFn>(GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
    shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(dll, "nvmlShutdown"));
    clock = reinterpret_cast<ClockFn>(GetProcAddress(dll, "nvmlDeviceGetClockInfo"));
    power = reinterpret_cast<PowerFn>(GetProcAddress(dll, "nvmlDeviceGetPowerUsage"));
    unsigned n = 0;
    if (!init || !count || !handle || !clock || init() != 0) {
      Close();
      return false;
    }
    if (count(&n) != 0 || n == 0 || handle(0, &device) != 0) {
      Close();
      return false;
    }
    return true;
  }
  void Sample(SystemSample* s) {
    unsigned mhz = 0, mw = 0;
    if (clock(device, 0 /* NVML_CLOCK_GRAPHICS */, &mhz) == 0) s->gpu_mhz = float(mhz);
    if (power && power(device, &mw) == 0) s->gpu_w = float(mw) / 1000.0f;
  }
  void Close() {
    if (shutdown && device) shutdown();
    if (dll) FreeLibrary(dll);
    dll = nullptr;
    device = nullptr;
  }
};

// Intel: the Graphics Control Library (ControlLib.dll, installed with the
// driver). Declarations follow its public API (igcl_api.h, version 1.1).
struct Igcl {
  struct InitArgs {
    uint32_t Size;
    uint8_t Version;
    uint32_t AppVersion;
    uint32_t flags;
    uint32_t SupportedVersion;
    uint32_t uid1;
    uint16_t uid2, uid3;
    uint8_t uid4[8];
  };
  struct FreqProperties {
    uint32_t Size;
    uint8_t Version;
    int32_t type;  // 0 GPU, 1 memory, 2 media
    bool canControl;
    double min, max;
  };
  struct FreqState {
    uint32_t Size;
    uint8_t Version;
    double currentVoltage, request, tdp, efficient, actual;
    uint32_t throttleReasons;
  };
  struct EnergyCounter {
    uint32_t Size;
    uint8_t Version;
    uint64_t energy;     // microjoules
    uint64_t timestamp;  // microseconds
  };
  using InitFn = int(__cdecl*)(InitArgs*, void**);
  using CloseFn = int(__cdecl*)(void*);
  using EnumDevicesFn = int(__cdecl*)(void*, uint32_t*, void**);
  using EnumFn = int(__cdecl*)(void*, uint32_t*, void**);
  using FreqPropsFn = int(__cdecl*)(void*, FreqProperties*);
  using FreqStateFn = int(__cdecl*)(void*, FreqState*);
  using EnergyFn = int(__cdecl*)(void*, EnergyCounter*);

  HMODULE dll = nullptr;
  void* api = nullptr;
  void* freq = nullptr;
  void* power = nullptr;
  CloseFn close = nullptr;
  FreqStateFn freq_state = nullptr;
  EnergyFn energy = nullptr;
  EnergyCounter last{};
  bool have_last = false;

  bool Open() {
    dll = LoadLibraryExW(L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return false;
    auto init = reinterpret_cast<InitFn>(GetProcAddress(dll, "ctlInit"));
    close = reinterpret_cast<CloseFn>(GetProcAddress(dll, "ctlClose"));
    auto enum_devices = reinterpret_cast<EnumDevicesFn>(GetProcAddress(dll, "ctlEnumerateDevices"));
    auto enum_freq = reinterpret_cast<EnumFn>(GetProcAddress(dll, "ctlEnumFrequencyDomains"));
    auto freq_props = reinterpret_cast<FreqPropsFn>(GetProcAddress(dll, "ctlFrequencyGetProperties"));
    freq_state = reinterpret_cast<FreqStateFn>(GetProcAddress(dll, "ctlFrequencyGetState"));
    auto enum_power = reinterpret_cast<EnumFn>(GetProcAddress(dll, "ctlEnumPowerDomains"));
    energy = reinterpret_cast<EnergyFn>(GetProcAddress(dll, "ctlPowerGetEnergyCounter"));
    if (!init || !enum_devices || !enum_freq || !freq_props || !freq_state) {
      Close();
      return false;
    }
    InitArgs args{};
    args.Size = sizeof(args);
    args.AppVersion = (1u << 16) | 1u;
    args.flags = 1u;  // CTL_INIT_FLAG_USE_LEVEL_ZERO: needed for telemetry
    if (init(&args, &api) != 0 || !api) {
      api = nullptr;
      Close();
      return false;
    }
    uint32_t n = 0;
    if (enum_devices(api, &n, nullptr) != 0 || n == 0) {
      Close();
      return false;
    }
    std::vector<void*> devices(n);
    if (enum_devices(api, &n, devices.data()) != 0) {
      Close();
      return false;
    }
    for (void* dev : devices) {
      uint32_t fn = 0;
      if (enum_freq(dev, &fn, nullptr) != 0 || fn == 0) continue;
      std::vector<void*> domains(fn);
      if (enum_freq(dev, &fn, domains.data()) != 0) continue;
      for (void* d : domains) {
        FreqProperties p{};
        p.Size = sizeof(p);
        if (freq_props(d, &p) == 0 && p.type == 0) {
          freq = d;
          break;
        }
      }
      if (!freq) continue;
      uint32_t pn = 0;
      if (enum_power && energy && enum_power(dev, &pn, nullptr) == 0 && pn > 0) {
        std::vector<void*> pd(pn);
        if (enum_power(dev, &pn, pd.data()) == 0) power = pd[0];
      }
      break;
    }
    if (!freq) {
      Close();
      return false;
    }
    return true;
  }
  void Sample(SystemSample* s) {
    FreqState st{};
    st.Size = sizeof(st);
    if (freq_state(freq, &st) == 0 && st.actual > 0) s->gpu_mhz = float(st.actual);
    if (power) {
      EnergyCounter e{};
      e.Size = sizeof(e);
      if (energy(power, &e) == 0) {
        if (have_last && e.timestamp > last.timestamp && e.energy >= last.energy) {
          s->gpu_w = float(double(e.energy - last.energy) / double(e.timestamp - last.timestamp));
        }
        last = e;
        have_last = true;
      }
    }
  }
  void Close() {
    if (api && close) close(api);
    api = nullptr;
    if (dll) FreeLibrary(dll);
    dll = nullptr;
  }
};

// Windows performance counters.
struct Pdh {
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER cpu_perf = nullptr, cpu_freq = nullptr, gpu_engine = nullptr, energy = nullptr;
  std::vector<uint8_t> buffer;

  void Open() {
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
      query = nullptr;
      return;
    }
    auto add = [&](const wchar_t* path, PDH_HCOUNTER* c) {
      if (PdhAddEnglishCounterW(query, path, 0, c) != ERROR_SUCCESS) *c = nullptr;
    };
    add(L"\\Processor Information(_Total)\\% Processor Performance", &cpu_perf);
    add(L"\\Processor Information(_Total)\\Processor Frequency", &cpu_freq);
    add(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", &gpu_engine);
    add(L"\\Energy Meter(*)\\Power", &energy);
    PdhCollectQueryData(query);
  }
  bool Value(PDH_HCOUNTER c, double* out) {
    if (!c) return false;
    PDH_FMT_COUNTERVALUE v{};
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) !=
            ERROR_SUCCESS ||
        (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA)) {
      return false;
    }
    *out = v.doubleValue;
    return true;
  }
  // Every instance's value (wildcard counters).
  bool Array(PDH_HCOUNTER c, std::vector<std::pair<std::wstring, double>>* out) {
    out->clear();
    if (!c) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size,
                                                 &count, nullptr);
    if (st != PDH_STATUS(PDH_MORE_DATA) || size == 0) return st == ERROR_SUCCESS;
    buffer.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                     items) != ERROR_SUCCESS) {
      return false;
    }
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
          items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
        out->emplace_back(items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue);
      }
    }
    return true;
  }
  // `gpu_pp1_w`: the RAPL graphics plane (integrated GPUs), < 0 none.
  void Sample(SystemSample* s, float* gpu_pp1_w) {
    *gpu_pp1_w = -1;
    if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) return;
    double perf = 0, freq = 0;
    if (Value(cpu_perf, &perf) && Value(cpu_freq, &freq) && freq > 0) {
      s->cpu_mhz = float(freq * perf / 100.0);
    }
    std::vector<std::pair<std::wstring, double>> items;
    // GPU: each adapter's 3D engine, summed over processes; the busiest one.
    if (Array(gpu_engine, &items) && !items.empty()) {
      std::map<std::wstring, double> per_engine;
      for (const auto& [name, v] : items) {
        const size_t luid = name.find(L"luid_");
        per_engine[luid == std::wstring::npos ? name : name.substr(luid)] += v;
      }
      double best = 0;
      for (const auto& [k, v] : per_engine) best = std::max(best, v);
      s->gpu_pct = float(std::min(best, 100.0));
    }
    // Energy Meter (RAPL): package and graphics plane, in milliwatts.
    if (Array(energy, &items)) {
      for (const auto& [name, v] : items) {
        std::wstring n = name;
        for (auto& ch : n) ch = wchar_t(towupper(ch));
        if (n.find(L"_PKG") != std::wstring::npos) {
          s->cpu_w = std::max(s->cpu_w, 0.0f) + float(v / 1000.0);
        } else if (n.find(L"_PP1") != std::wstring::npos) {
          *gpu_pp1_w = std::max(*gpu_pp1_w, 0.0f) + float(v / 1000.0);
        }
      }
    }
  }
  void Close() {
    if (query) PdhCloseQuery(query);
    query = nullptr;
  }
};

uint64_t FileTime(const FILETIME& f) {
  return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

#endif  // _WIN32

// Samples the system counters twice a second while the panel is open.
class Sampler {
 public:
  Sampler() { thread_ = std::thread([this] { Run(); }); }
  ~Sampler() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
#if defined(_WIN32)
    Pdh pdh;
    pdh.Open();
    std::string name;
    {
      std::lock_guard lock(g_gpu_name_mutex);
      name = g_gpu_name;
    }
    auto has = [&](const char* s) {
      std::string n = name;
      for (auto& ch : n) ch = char(std::tolower(uint8_t(ch)));
      return n.find(s) != std::string::npos;
    };
    // The vendor library for the GPU the native renderer uses (else the first
    // one that loads).
    Nvml nvml;
    Igcl igcl;
    bool use_nvml = false, use_igcl = false;
    const bool known = !name.empty();
    if (!known || has("nvidia")) use_nvml = nvml.Open();
    if (!use_nvml && (!known || has("intel"))) use_igcl = igcl.Open();
    REXLOG_INFO("Performance overlay: GPU clock and power from {}",
                use_nvml ? "NVML" : use_igcl ? "Intel IGCL" : "nowhere (no vendor library)");
    FILETIME idle0{}, kernel0{}, user0{}, pc{}, pe{}, pk0{}, pu0{};
    GetSystemTimes(&idle0, &kernel0, &user0);
    GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk0, &pu0);
    auto t_prev = Clock::now();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const double cpus = std::max<DWORD>(si.dwNumberOfProcessors, 1);
#endif
    while (true) {
      {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_; })) break;
      }
      SystemSample s;
#if defined(_WIN32)
      FILETIME idle{}, kernel{}, user{}, pk{}, pu{};
      GetSystemTimes(&idle, &kernel, &user);
      GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu);
      const auto t_now = Clock::now();
      const double wall_100ns =
          std::chrono::duration<double>(t_now - t_prev).count() * 1e7;
      t_prev = t_now;
      const double d_idle = double(FileTime(idle) - FileTime(idle0));
      const double d_total = double(FileTime(kernel) - FileTime(kernel0)) +
                             double(FileTime(user) - FileTime(user0));  // kernel includes idle
      if (d_total > 0) s.cpu_pct = float(std::clamp(100.0 * (d_total - d_idle) / d_total, 0.0, 100.0));
      const double d_proc = double(FileTime(pk) - FileTime(pk0)) + double(FileTime(pu) - FileTime(pu0));
      if (wall_100ns > 0) s.proc_pct = float(std::clamp(100.0 * d_proc / (wall_100ns * cpus), 0.0, 100.0));
      idle0 = idle;
      kernel0 = kernel;
      user0 = user;
      pk0 = pk;
      pu0 = pu;
      float pp1 = -1;
      pdh.Sample(&s, &pp1);
      if (use_nvml) {
        nvml.Sample(&s);
        s.gpu_source = "NVML";
      } else if (use_igcl) {
        igcl.Sample(&s);
        s.gpu_source = "IGCL";
      }
      if (s.gpu_w < 0 && pp1 >= 0) s.gpu_w = pp1;  // integrated: RAPL graphics plane
#endif
      std::lock_guard lock(g_sys_mutex);
      g_sys = s;
    }
#if defined(_WIN32)
    pdh.Close();
    if (use_nvml) nvml.Close();
    if (use_igcl) igcl.Close();
#endif
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

// --- The panel ------------------------------------------------------------------

std::string Num(float v, const char* fmt_str, const char* unit) {
  if (v < 0) return "n/a";
  return fmt::format(fmt::runtime(fmt_str), v) + unit;
}

class PerfOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    sampler_ = std::make_unique<Sampler>();
  }
  ~PerfOverlay() override = default;

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    // Copy the history, oldest first.
    std::vector<FrameSample> h;
    {
      std::lock_guard lock(g_frames_mutex);
      h.reserve(g_frames_count);
      const size_t start = (g_frames_next + kHistory - g_frames_count) % kHistory;
      for (size_t i = 0; i < g_frames_count; ++i) h.push_back(g_frames[(start + i) % kHistory]);
    }
    SystemSample sys;
    {
      std::lock_guard lock(g_sys_mutex);
      sys = g_sys;
    }
    // Averages over the last half second of frames.
    float frame = 0, cpu = 0, render = 0, gpu = 0, worst = 0;
    int n = 0, n_render = 0, n_gpu = 0;
    float span = 0;
    for (size_t i = h.size(); i-- > 0 && span < 500.0f;) {
      const auto& f = h[i];
      span += f.frame;
      frame += f.frame;
      cpu += f.cpu;
      worst = std::max(worst, f.frame);
      if (f.render >= 0) {
        render += f.render;
        ++n_render;
      }
      if (f.gpu >= 0) {
        gpu += f.gpu;
        ++n_gpu;
      }
      ++n;
    }
    // 1% low: the frame rate of the slowest 1% of the last few seconds.
    float low_fps = -1;
    if (h.size() >= 60) {
      std::vector<float> t;
      t.reserve(h.size());
      for (const auto& f : h) t.push_back(f.frame);
      std::sort(t.begin(), t.end(), std::greater<float>());
      const size_t k = std::max<size_t>(1, t.size() / 100);
      float sum = 0;
      for (size_t i = 0; i < k; ++i) sum += t[i];
      if (sum > 0) low_fps = 1000.0f * float(k) / sum;
    }

    // Frames the native renderer showed (fewer than the game's when it can't
    // keep up: the cause of uneven motion at a steady game frame rate).
    {
      const auto now = Clock::now();
      const uint64_t shown = g_presented.load(std::memory_order_relaxed);
      if (shown_t_ == Clock::time_point{}) {
        shown_t_ = now;
        shown_count_ = shown;
      } else if (now - shown_t_ >= std::chrono::milliseconds(500)) {
        const double s = std::chrono::duration<double>(now - shown_t_).count();
        shown_fps_ = shown != shown_count_ ? float(double(shown - shown_count_) / s) : -1.0f;
        shown_t_ = now;
        shown_count_ = shown;
      }
    }
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Performance##amped3_perf", nullptr, flags)) {
      const ImVec4 kFrameCol(0.92f, 0.92f, 0.92f, 1.0f);
      const ImVec4 kCpuCol(0.40f, 0.72f, 1.00f, 1.0f);
      const ImVec4 kGpuCol(1.00f, 0.62f, 0.25f, 1.0f);
      const ImVec4 kRenderCol(0.78f, 0.55f, 1.00f, 1.0f);
      const ImVec4 kDim(0.65f, 0.65f, 0.65f, 1.0f);
      if (n > 0 && frame > 0) {
        const float avg = frame / float(n);
        ImGui::Text("%.1f fps", 1000.0f / avg);
        ImGui::SameLine();
        if (shown_fps_ >= 0) {
          const bool short_of = shown_fps_ < 1000.0f / avg - 1.5f;
          ImGui::TextColored(short_of ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : kDim, "(%.1f shown)",
                             shown_fps_);
          ImGui::SameLine();
        }
        ImGui::TextColored(kFrameCol, "  frame %.2f ms", avg);
        ImGui::SameLine();
        ImGui::TextColored(kDim, "  worst %.1f ms", worst);
        if (low_fps > 0) {
          ImGui::SameLine();
          ImGui::TextColored(kDim, "  1%% low %.0f fps", low_fps);
        }
        ImGui::TextColored(kCpuCol, "CPU frame %.2f ms", cpu / float(n));
        ImGui::SameLine();
        ImGui::TextColored(kDim, " (game thread)");
        if (n_render > 0) {
          ImGui::SameLine();
          ImGui::TextColored(kRenderCol, " renderer thread %.2f ms", render / float(n_render));
        }
        if (n_gpu > 0) {
          ImGui::TextColored(kGpuCol, "GPU frame %.2f ms", gpu / float(n_gpu));
        } else {
          ImGui::TextColored(kGpuCol, "GPU frame n/a");
          ImGui::SameLine();
          ImGui::TextColored(kDim, " (needs the native renderer)");
        }
      } else {
        ImGui::TextUnformatted("Waiting for frames...");
      }
      DrawGraph(h, kFrameCol, kCpuCol, kGpuCol, kRenderCol);
      ImGui::Separator();
      if (ImGui::BeginTable("##perf_sys", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kCpuCol, "CPU");
        ImGui::TableNextColumn();
        ImGui::Text("%s", Num(sys.cpu_pct, "{:.0f}", "%").c_str());
        if (sys.proc_pct >= 0) {
          ImGui::SameLine();
          ImGui::TextColored(kDim, "(game %.0f%%)", sys.proc_pct);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%s", sys.cpu_mhz > 0 ? fmt::format("{:.2f} GHz", sys.cpu_mhz / 1000.0f).c_str()
                                          : "n/a");
        ImGui::TableNextColumn();
        ImGui::Text("%s", Num(sys.cpu_w, "{:.1f}", " W").c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kGpuCol, "GPU");
        ImGui::TableNextColumn();
        ImGui::Text("%s", Num(sys.gpu_pct, "{:.0f}", "%").c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s", sys.gpu_mhz > 0 ? fmt::format("{:.2f} GHz", sys.gpu_mhz / 1000.0f).c_str()
                                          : "n/a");
        ImGui::TableNextColumn();
        ImGui::Text("%s", Num(sys.gpu_w, "{:.1f}", " W").c_str());
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }

 private:
  void DrawGraph(const std::vector<FrameSample>& h, const ImVec4& frame_col, const ImVec4& cpu_col,
                 const ImVec4& gpu_col, const ImVec4& render_col) {
    const float w = 360.0f, ht = 96.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, ht));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + ht), IM_COL32(0, 0, 0, 110), 3.0f);
    // Scale: at least 33.3 ms, else the recent peak rounded up to 10 ms
    // (eased, so a single spike doesn't make it jump).
    float peak = 0;
    for (const auto& f : h) peak = std::max({peak, f.frame, f.cpu, f.gpu, f.render});
    const float target = std::min(std::max(33.4f, std::ceil(peak / 10.0f) * 10.0f), 200.0f);
    scale_ = scale_ <= 0 ? target : scale_ + (target - scale_) * 0.1f;
    auto y_of = [&](float ms) { return p0.y + ht - std::clamp(ms / scale_, 0.0f, 1.0f) * (ht - 2); };
    // Reference lines: 60 and 30 fps.
    for (float ref : {16.67f, 33.33f}) {
      if (ref >= scale_) continue;
      const float y = y_of(ref);
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + w, y), IM_COL32(255, 255, 255, 40));
      char label[16];
      std::snprintf(label, sizeof(label), "%.0f fps", 1000.0f / ref);
      dl->AddText(ImVec2(p0.x + 3, y - ImGui::GetTextLineHeight()), IM_COL32(255, 255, 255, 90),
                  label);
    }
    char top[16];
    std::snprintf(top, sizeof(top), "%.0f ms", scale_);
    dl->AddText(ImVec2(p0.x + w - ImGui::CalcTextSize(top).x - 3, p0.y + 1),
                IM_COL32(255, 255, 255, 90), top);
    if (h.size() < 2) return;
    const float dx = w / float(kHistory - 1);
    const float x0 = p0.x + w - dx * float(h.size() - 1);
    auto line = [&](auto get, const ImVec4& col, float thick) {
      std::vector<ImVec2> pts;
      pts.reserve(h.size());
      auto flush = [&] {
        if (pts.size() >= 2) {
          dl->AddPolyline(pts.data(), int(pts.size()), ImGui::ColorConvertFloat4ToU32(col),
                          ImDrawFlags_None, thick);
        }
        pts.clear();
      };
      for (size_t i = 0; i < h.size(); ++i) {
        const float v = get(h[i]);
        if (v < 0) {
          flush();
          continue;
        }
        pts.emplace_back(x0 + dx * float(i), y_of(v));
      }
      flush();
    };
    line([](const FrameSample& f) { return f.render; }, render_col, 1.5f);
    line([](const FrameSample& f) { return f.gpu; }, gpu_col, 1.5f);
    line([](const FrameSample& f) { return f.cpu; }, cpu_col, 1.5f);
    line([](const FrameSample& f) { return f.frame; }, frame_col, 1.5f);
    // Legend.
    ImGui::TextColored(frame_col, "frame");
    ImGui::SameLine();
    ImGui::TextColored(cpu_col, "CPU");
    ImGui::SameLine();
    ImGui::TextColored(gpu_col, "GPU");
    ImGui::SameLine();
    ImGui::TextColored(render_col, "renderer thread");
  }

  std::unique_ptr<Sampler> sampler_;
  float scale_ = 0;
  Clock::time_point shown_t_{};
  uint64_t shown_count_ = 0;
  float shown_fps_ = -1;
};

std::mutex g_overlay_mutex;
std::unique_ptr<PerfOverlay> g_overlay;
rex::ui::ImGuiDrawer* g_drawer = nullptr;

}  // namespace

void OnSwapBegin() {
  const auto now = Clock::now();
  const uint64_t cycles = ThreadCycles();
  const uint64_t tsc = Tsc();
  if (g_tsc_start == 0) {
    g_tsc_start = tsc;
    g_tsc_start_time = now;
  } else {
    const double ms = std::chrono::duration<double, std::milli>(now - g_tsc_start_time).count();
    if (ms > 200.0) g_tsc_per_ms = double(tsc - g_tsc_start) / ms;
  }
  if (g_last_begin != Clock::time_point{} && g_have_end) {
    FrameSample s;
    s.frame = float(std::chrono::duration<double, std::milli>(now - g_last_begin).count());
    const double per_ms = g_tsc_per_ms.load();
#if defined(_WIN32)
    s.cpu = per_ms > 0 ? float(double(cycles - g_end_cycles) / per_ms) : 0.0f;
#else
    s.cpu = float(double(cycles - g_end_cycles) * 1e-6);
    (void)per_ms;
#endif
    s.cpu = std::min(s.cpu, s.frame);
    const int64_t now_ns = NowNs();
    constexpr int64_t kStale = 500'000'000;  // reports older than this: the renderer is off
    if (now_ns - g_render_at.load() < kStale) s.render = float(g_render_ms.load());
    if (now_ns - g_gpu_at.load() < kStale) s.gpu = float(g_gpu_ms.load());
    if (s.frame > 0 && s.frame < 5000.0f) {
      std::lock_guard lock(g_frames_mutex);
      g_frames[g_frames_next] = s;
      g_frames_next = (g_frames_next + 1) % kHistory;
      g_frames_count = std::min(g_frames_count + 1, kHistory);
    }
  }
  g_last_begin = now;
}

void OnSwapEnd() {
  g_end_cycles = ThreadCycles();
  g_have_end = true;
}

void ReportRenderFrame(double worker_ms) {
  amped3::trace::OnRenderFrame(worker_ms);
  g_render_ms = worker_ms;
  g_render_at = NowNs();
}

void ReportGpuFrame(double gpu_ms) {
  amped3::trace::OnGpuFrame(gpu_ms);
  if (gpu_ms < 0) return;
  g_gpu_ms = gpu_ms;
  g_gpu_at = NowNs();
}

void ReportPresented() {
  amped3::trace::OnPresented();
  g_presented.fetch_add(1, std::memory_order_relaxed);
}

void SetGpuName(const std::string& name) {
  std::lock_guard lock(g_gpu_name_mutex);
  g_gpu_name = name;
}

void Install(rex::ui::ImGuiDrawer* drawer) {
  g_drawer = drawer;
  // Takes over the SDK's debug overlay key (its panel shows only the frame
  // rate); the key is the one set for that, F3 by default.
  std::string key = rex::cvar::GetFlagByName("bind_debug_overlay");
  if (key.empty()) key = "F3";
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::RegisterBind(kBind, key, "Show or hide the performance overlay", [] {
    std::lock_guard lock(g_overlay_mutex);
    if (g_overlay) {
      g_overlay.reset();
    } else if (g_drawer) {
      g_overlay = std::make_unique<PerfOverlay>(g_drawer);
    }
  });
}

void Shutdown() {
  rex::ui::UnregisterBind(kBind);
  std::lock_guard lock(g_overlay_mutex);
  g_overlay.reset();
  g_drawer = nullptr;
}

}  // namespace amped3::perf
