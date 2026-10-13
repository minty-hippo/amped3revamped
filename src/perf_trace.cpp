// amped3 - performance trace (see perf_trace.h).

#include "perf_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(perf_trace, false, "Amped 3/Diagnostics",
                    "Records a performance trace to logs (perf_<time>.csv, _threads.csv, "
                    "_summary.txt) for perf_trace_seconds, then switches itself back off. Also F11.");
REXCVAR_DEFINE_INT32(perf_trace_seconds, 20, "Amped 3/Diagnostics",
                     "How long a performance trace records (seconds)")
    .range(2, 300);

namespace amped3::trace {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kZones = int(Zone::kCount);
constexpr const char* kBind = "bind_perf_trace";

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

uint64_t CurrentTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return uint64_t(syscall(SYS_gettid));
#endif
}

// --- Trace state -----------------------------------------------------------------

std::atomic<bool> g_active{false};
std::atomic<int64_t> g_zone_ns[kZones];

struct Row {
  double t_ms = 0;         // since the trace started
  double frame_ms = 0;     // swap to swap
  double swap_cpu_ms = 0;  // CPU time of the swapping thread over the frame
  std::array<double, kZones> zone_ms{};
  double worker_ms = -1;  // native renderer worker, average over reports in the frame
  double gpu_ms = -1;
  int presents = 0;
};

std::mutex g_mutex;  // rows, presents, thread ids
std::vector<Row> g_rows;
std::vector<int64_t> g_present_ns;
int64_t g_start_ns = 0;
uint64_t g_swap_tid = 0, g_worker_tid = 0;

// Worker / GPU reports since the last swap.
std::atomic<int64_t> g_worker_sum_us{0}, g_gpu_sum_us{0};
std::atomic<int> g_worker_count{0}, g_gpu_count{0}, g_present_count{0};

// Swap thread only.
bool t_have_prev = false;
int64_t t_prev_ns = 0, t_prev_cpu_ns = 0;
std::array<int64_t, kZones> t_prev_zone{};

// A trace's sampler thread runs detached (a process can end while one
// records); this stays set until it has written its files.
std::atomic<bool> g_running{false};

// --- Threads ---------------------------------------------------------------------

struct ThreadSample {
  std::string name;
  int64_t cpu_ns = 0;
};
using ThreadMap = std::map<uint64_t, ThreadSample>;

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(n - 1, 0)), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

ThreadMap SampleThreads() {
  ThreadMap out;
  using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_desc = reinterpret_cast<GetDescFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c, e, k, u;
    ThreadSample s;
    if (GetThreadTimes(h, &c, &e, &k, &u)) {
      const auto v = [](const FILETIME& f) {
        return (int64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 100;
      };
      s.cpu_ns = v(k) + v(u);
    }
    if (get_desc) {
      PWSTR desc = nullptr;
      if (SUCCEEDED(get_desc(h, &desc)) && desc) {
        s.name = WideToUtf8(desc);
        LocalFree(desc);
      }
    }
    CloseHandle(h);
    out[te.th32ThreadID] = std::move(s);
  }
  CloseHandle(snap);
  return out;
}
#else
std::string ReadFirstLine(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

ThreadMap SampleThreads() {
  ThreadMap out;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return out;
  static const long ticks = sysconf(_SC_CLK_TCK);
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const std::string base = std::string("/proc/self/task/") + e->d_name;
    ThreadSample s;
    s.name = ReadFirstLine(base + "/comm");
    // schedstat: nanoseconds on a CPU (precise); stat: utime + stime in ticks.
    const std::string sched = ReadFirstLine(base + "/schedstat");
    if (!sched.empty()) {
      s.cpu_ns = std::strtoll(sched.c_str(), nullptr, 10);
    } else {
      const std::string stat = ReadFirstLine(base + "/stat");
      const size_t close = stat.rfind(')');
      if (close != std::string::npos && ticks > 0) {
        // Fields after the name: state is field 3; utime 14, stime 15.
        const char* p = stat.c_str() + close + 2;
        long long utime = 0, stime = 0;
        int field = 3;
        while (*p && field < 16) {
          if (field == 14) utime = std::strtoll(p, nullptr, 10);
          if (field == 15) stime = std::strtoll(p, nullptr, 10);
          while (*p && *p != ' ') ++p;
          while (*p == ' ') ++p;
          ++field;
        }
        s.cpu_ns = (utime + stime) * (1'000'000'000LL / ticks);
      }
    }
    out[std::strtoull(e->d_name, nullptr, 10)] = std::move(s);
  }
  closedir(dir);
  return out;
}
#endif

// --- Writing ---------------------------------------------------------------------

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double idx = p * double(v.size() - 1);
  const size_t lo = size_t(idx), hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

struct ThreadStat {
  uint64_t tid;
  std::string name, role;
  double cpu_ms = 0, avg_core_pct = 0, max_core_pct = 0;
};

void WriteResults(double seconds, const std::vector<ThreadMap>& samples,
                  const std::vector<int64_t>& sample_ns) {
  std::vector<Row> rows;
  std::vector<int64_t> presents;
  uint64_t swap_tid, worker_tid;
  {
    std::lock_guard lock(g_mutex);
    rows = g_rows;
    presents = g_present_ns;
    swap_tid = g_swap_tid;
    worker_tid = g_worker_tid;
  }
  const auto dir = rex::filesystem::GetExecutableFolder() / "logs";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string stamp = Stamp();
  const auto base = dir / ("perf_" + stamp);

  // Per frame.
  {
    std::ofstream f(base.string() + ".csv");
    f << "t_ms,frame_ms,swap_thread_cpu_ms";
    for (int z = 0; z < kZones; ++z) f << "," << ZoneName(Zone(z)) << "_ms";
    f << ",worker_cpu_ms,gpu_ms,presents\n";
    for (const Row& r : rows) {
      f << fmt::format("{:.2f},{:.3f},{:.3f}", r.t_ms, r.frame_ms, r.swap_cpu_ms);
      for (double z : r.zone_ms) f << fmt::format(",{:.3f}", z);
      f << fmt::format(",{:.3f},{:.3f},{}\n", r.worker_ms, r.gpu_ms, r.presents);
    }
  }

  // Per thread: totals over the trace and the busiest second.
  std::vector<ThreadStat> threads;
  if (samples.size() >= 2) {
    const ThreadMap& first = samples.front();
    const ThreadMap& last = samples.back();
    const double span_ms = double(sample_ns.back() - sample_ns.front()) * 1e-6;
    for (const auto& [tid, s] : last) {
      ThreadStat t;
      t.tid = tid;
      t.name = s.name;
      if (tid == swap_tid) t.role = "game: calls D3DDevice_Swap";
      if (tid == worker_tid) t.role = "native renderer worker";
      const auto it = first.find(tid);
      const int64_t start = it != first.end() ? it->second.cpu_ns : 0;
      t.cpu_ms = double(s.cpu_ns - start) * 1e-6;
      t.avg_core_pct = span_ms > 0 ? 100.0 * t.cpu_ms / span_ms : 0;
      for (size_t i = 1; i < samples.size(); ++i) {
        const auto a = samples[i - 1].find(tid), b = samples[i].find(tid);
        if (b == samples[i].end()) continue;
        const int64_t before = a != samples[i - 1].end() ? a->second.cpu_ns : 0;
        const double dt = double(sample_ns[i] - sample_ns[i - 1]) * 1e-6;
        if (dt > 0) {
          t.max_core_pct = std::max(t.max_core_pct, 100.0 * double(b->second.cpu_ns - before) * 1e-6 / dt);
        }
      }
      threads.push_back(std::move(t));
    }
    std::sort(threads.begin(), threads.end(),
              [](const ThreadStat& a, const ThreadStat& b) { return a.cpu_ms > b.cpu_ms; });
    std::ofstream f(base.string() + "_threads.csv");
    f << "tid,name,role,cpu_ms,avg_core_pct,max_core_pct_1s\n";
    for (const auto& t : threads) {
      f << fmt::format("{},\"{}\",\"{}\",{:.1f},{:.1f},{:.1f}\n", t.tid, t.name, t.role, t.cpu_ms,
                       t.avg_core_pct, t.max_core_pct);
    }
  }

  // Summary.
  std::string s;
  s += fmt::format("Amped 3 Revamped performance trace {} ({:.0f} s, {} frames)\n", stamp, seconds,
                   rows.size());
  s += fmt::format("Settings: native_renderer={} revamped_graphics={} frame_rate_limit={} gpu_backend={}\n",
                   rex::cvar::GetFlagByName("native_renderer"),
                   rex::cvar::GetFlagByName("revamped_graphics"),
                   rex::cvar::GetFlagByName("frame_rate_limit"),
                   rex::cvar::GetFlagByName("gpu_backend"));
  if (!rows.empty()) {
    std::vector<double> frame, cpu;
    std::array<double, kZones> zone_sum{};
    double worker_sum = 0, gpu_sum = 0;
    int worker_n = 0, gpu_n = 0;
    for (const Row& r : rows) {
      frame.push_back(r.frame_ms);
      cpu.push_back(r.swap_cpu_ms);
      for (int z = 0; z < kZones; ++z) zone_sum[z] += r.zone_ms[z];
      if (r.worker_ms >= 0) worker_sum += r.worker_ms, ++worker_n;
      if (r.gpu_ms >= 0) gpu_sum += r.gpu_ms, ++gpu_n;
    }
    const double n = double(rows.size());
    double total = 0;
    for (double v : frame) total += v;
    const double p50 = Percentile(frame, 0.5);
    const double stutter_at = std::max(p50 * 1.5, p50 + 4.0);
    const auto stutters = std::count_if(frame.begin(), frame.end(),
                                        [&](double v) { return v > stutter_at; });
    s += fmt::format(
        "Game frames: {:.1f} fps; frame time avg {:.2f} ms, p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, "
        "max {:.2f}; {} stutters (> {:.1f} ms)\n",
        total > 0 ? 1000.0 * n / total : 0, total / n, p50, Percentile(frame, 0.95),
        Percentile(frame, 0.99), Percentile(frame, 1.0), stutters, stutter_at);
    double cpu_total = 0;
    for (double v : cpu) cpu_total += v;
    s += fmt::format("Swapping thread CPU: avg {:.2f} ms/frame, p95 {:.2f}\n", cpu_total / n,
                     Percentile(cpu, 0.95));
    s += "Zones (avg ms per frame, share of frame time):\n";
    for (int z = 0; z < kZones; ++z) {
      s += fmt::format("  {:<12} {:7.3f} ms  {:5.1f}%\n", ZoneName(Zone(z)), zone_sum[z] / n,
                       total > 0 ? 100.0 * zone_sum[z] / total : 0);
    }
    s += fmt::format("Native renderer worker: {}\n",
                     worker_n ? fmt::format("{:.2f} ms CPU per frame", worker_sum / worker_n)
                              : std::string("no frames (renderer off?)"));
    s += fmt::format("GPU: {}\n", gpu_n ? fmt::format("{:.2f} ms per frame", gpu_sum / gpu_n)
                                        : std::string("no timings"));
  }
  if (presents.size() >= 3) {
    std::vector<double> gaps;
    for (size_t i = 1; i < presents.size(); ++i) gaps.push_back(double(presents[i] - presents[i - 1]) * 1e-6);
    const double p50 = Percentile(gaps, 0.5);
    const auto uneven = std::count_if(gaps.begin(), gaps.end(),
                                      [&](double v) { return v > std::max(p50 * 1.5, p50 + 4.0); });
    s += fmt::format(
        "Presented frames: {}; interval p50 {:.2f} ms, p95 {:.2f}, p99 {:.2f}, max {:.2f}; {} "
        "uneven\n",
        presents.size(), p50, Percentile(gaps, 0.95), Percentile(gaps, 0.99),
        Percentile(gaps, 1.0), uneven);
  }
  if (!threads.empty()) {
    s += "Busiest threads (share of one core: average, busiest second):\n";
    for (size_t i = 0; i < threads.size() && i < 12; ++i) {
      const auto& t = threads[i];
      s += fmt::format("  {:>7} {:<16} {:5.1f}% {:5.1f}%  {}\n", t.tid, t.name, t.avg_core_pct,
                       t.max_core_pct, t.role);
    }
  }
  {
    std::ofstream f(base.string() + "_summary.txt");
    f << s;
  }
  REXLOG_INFO("Performance trace written to {}_*.csv / _summary.txt:\n{}", base.string(), s);
}

void RunTrace() {
  const int seconds = std::max(2, REXCVAR_GET(perf_trace_seconds));
  std::vector<ThreadMap> samples;
  std::vector<int64_t> sample_ns;
  samples.push_back(SampleThreads());
  sample_ns.push_back(NowNs());
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  while (Clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    samples.push_back(SampleThreads());
    sample_ns.push_back(NowNs());
  }
  g_active.store(false, std::memory_order_release);
  WriteResults(double(seconds), samples, sample_ns);
  rex::cvar::SetFlagByName("perf_trace", "false");
  g_running.store(false, std::memory_order_release);
}

}  // namespace

const char* ZoneName(Zone zone) {
  switch (zone) {
    case Zone::kD3D: return "d3d";
    case Zone::kFlushState: return "flush_state";
    case Zone::kRecord: return "record";
    case Zone::kFenceWait: return "fence_wait";
    case Zone::kMakeSpace: return "make_space";
    case Zone::kSwap: return "swap";
    case Zone::kLimiter: return "limiter";
    default: return "?";
  }
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

void Add(Zone zone, int64_t ns) {
  if (Active()) g_zone_ns[int(zone)].fetch_add(ns, std::memory_order_relaxed);
}

Scope::Scope(Zone zone) : zone_(zone) {
  if (Active()) start_ = NowNs();
}

Scope::~Scope() {
  if (start_) Add(zone_, NowNs() - start_);
}

int64_t ThreadCpuNs() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  const auto v = [](const FILETIME& f) {
    return (int64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 100;
  };
  return v(k) + v(u);
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
#endif
}

void Start() {
  if (g_running.exchange(true)) return;  // one at a time
  {
    std::lock_guard rows_lock(g_mutex);
    g_rows.clear();
    g_present_ns.clear();
    g_start_ns = NowNs();
  }
  g_worker_sum_us = 0;
  g_gpu_sum_us = 0;
  g_worker_count = 0;
  g_gpu_count = 0;
  g_present_count = 0;
  REXLOG_INFO("Performance trace: recording {} s", REXCVAR_GET(perf_trace_seconds));
  g_active.store(true, std::memory_order_release);
  std::thread(RunTrace).detach();
}

void OnSwap() {
  const int64_t now = NowNs();
  if (!Active()) {
    t_have_prev = false;
    return;
  }
  const int64_t cpu = ThreadCpuNs();
  std::array<int64_t, kZones> zone{};
  for (int z = 0; z < kZones; ++z) zone[z] = g_zone_ns[z].load(std::memory_order_relaxed);
  const int worker_n = g_worker_count.exchange(0);
  const int64_t worker_us = g_worker_sum_us.exchange(0);
  const int gpu_n = g_gpu_count.exchange(0);
  const int64_t gpu_us = g_gpu_sum_us.exchange(0);
  const int presents = g_present_count.exchange(0);
  if (t_have_prev) {
    Row r;
    r.t_ms = double(now - g_start_ns) * 1e-6;
    r.frame_ms = double(now - t_prev_ns) * 1e-6;
    r.swap_cpu_ms = double(cpu - t_prev_cpu_ns) * 1e-6;
    for (int z = 0; z < kZones; ++z) r.zone_ms[z] = double(zone[z] - t_prev_zone[z]) * 1e-6;
    if (worker_n) r.worker_ms = double(worker_us) * 1e-3 / worker_n;
    if (gpu_n) r.gpu_ms = double(gpu_us) * 1e-3 / gpu_n;
    r.presents = presents;
    std::lock_guard lock(g_mutex);
    g_rows.push_back(r);
    g_swap_tid = CurrentTid();
  }
  t_have_prev = true;
  t_prev_ns = now;
  t_prev_cpu_ns = cpu;
  t_prev_zone = zone;
}

void OnRenderFrame(double worker_ms) {
  if (!Active()) return;
  g_worker_sum_us.fetch_add(int64_t(worker_ms * 1000.0), std::memory_order_relaxed);
  g_worker_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  g_worker_tid = CurrentTid();
}

void OnGpuFrame(double gpu_ms) {
  if (!Active() || gpu_ms < 0) return;
  g_gpu_sum_us.fetch_add(int64_t(gpu_ms * 1000.0), std::memory_order_relaxed);
  g_gpu_count.fetch_add(1, std::memory_order_relaxed);
}

void OnPresented() {
  if (!Active()) return;
  g_present_count.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  g_present_ns.push_back(NowNs());
}

void Install() {
  // A trace still on from a session that ended while recording.
  if (REXCVAR_GET(perf_trace)) rex::cvar::SetFlagByName("perf_trace", "false");
  rex::ui::RegisterBind(kBind, "F11", "Record a performance trace to logs (perf_trace_seconds)",
                        [] { Start(); });
  rex::cvar::RegisterChangeCallback("perf_trace", [](std::string_view, std::string_view value) {
    if (value == "true") Start();
  });
}

}  // namespace amped3::trace
