// amped3 - crash reporter. See crash_report.h.

#include "crash_report.h"

#include <rex/logging.h>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dbghelp.h and tlhelp32.h need windows.h first.
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace amped3::crash {

namespace {

std::filesystem::path g_log_dir;
const uint8_t* g_vbase = nullptr;
const uint8_t* g_pbase = nullptr;
std::atomic<bool> g_in_handler{false};
std::function<std::string()> g_hang_extra;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

constexpr uint64_t kArenaSize = 0x100000000ull;  // 4 GB guest address space

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
  }
}

std::string GuestNote(uint64_t addr) {
  auto a = static_cast<uint64_t>(addr);
  if (g_vbase) {
    auto v = reinterpret_cast<uint64_t>(g_vbase);
    if (a >= v && a < v + kArenaSize) return fmt::format(" (guest 0x{:08X})", a - v);
  }
  if (g_pbase) {
    auto p = reinterpret_cast<uint64_t>(g_pbase);
    if (a >= p && a < p + kArenaSize) return fmt::format(" (guest physical 0x{:08X})", a - p);
  }
  return {};
}

// "amped3.exe+0x1234 sub_8274BD28+0x40 [file.cpp:123]"
std::string Describe(HANDLE process, uint64_t addr) {
  std::string out = fmt::format("0x{:016X}", addr);
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(addr), &module)) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string name = std::filesystem::path(path).filename().string();
    out += fmt::format("  {}+0x{:X}", name, addr - reinterpret_cast<uint64_t>(module));
  }
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, addr, &displacement, symbol)) {
    out += fmt::format("  {}+0x{:X}", symbol->Name, displacement);
  }
  IMAGEHLP_LINE64 line = {};
  line.SizeOfStruct = sizeof(line);
  DWORD line_disp = 0;
  if (SymGetLineFromAddr64(process, addr, &line_disp, &line) && line.FileName) {
    out += fmt::format("  [{}:{}]", std::filesystem::path(line.FileName).filename().string(),
                       line.LineNumber);
  }
  return out;
}

void InitSymbols(HANDLE process) {
  static std::atomic<bool> done{false};
  if (!done.exchange(true)) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
  }
}

long long Stamp() {
  return std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds(1);
}

// Logs the report and also writes it as amped3_<kind>_<time>.txt, in case
// logging itself is what broke.
void WriteReport(const char* kind, long long stamp, const std::string& report) {
  REXLOG_CRITICAL("{}", report);
  rex::FlushLogging();
  const auto txt_path = g_log_dir / fmt::format("amped3_{}_{}.txt", kind, stamp);
  if (FILE* f = _wfopen(txt_path.c_str(), L"w")) {
    std::fputs(report.c_str(), f);
    std::fclose(f);
  }
}

// Stack of the calling thread, minus `skip` frames of this file.
std::string CurrentStack(int skip) {
  HANDLE process = GetCurrentProcess();
  InitSymbols(process);
  void* frames[62] = {};
  const USHORT count = CaptureStackBackTrace(static_cast<DWORD>(skip), 62, frames, nullptr);
  std::string out = "  stack:\n";
  for (USHORT i = 0; i < count; ++i) {
    out += fmt::format("    #{:<2} {}\n", i,
                       Describe(process, reinterpret_cast<uint64_t>(frames[i])));
  }
  return out;
}

// exit() / abort() / std::terminate: these end the process without an
// exception, so the unhandled-exception filter never sees them.
void ReportNonException(const char* kind, const char* what) {
  if (g_in_handler.exchange(true)) {
    return;
  }
  std::string report = fmt::format("[{}] {} on thread {}\n", kind, what, GetCurrentThreadId());
  report += CurrentStack(2);
  WriteReport(kind, Stamp(), report);
}

void OnExit() { ReportNonException("exit", "process exit() called"); }
void OnAbort(int) { ReportNonException("abort", "abort() called (SIGABRT)"); }
[[noreturn]] void OnTerminate() {
  ReportNonException("terminate", "std::terminate called (uncaught C++ exception?)");
  std::abort();
}
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  ReportNonException("invalid_parameter", "C runtime invalid-parameter handler");
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
  if (g_in_handler.exchange(true)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  InitSymbols(process);

  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  const auto pc = reinterpret_cast<uint64_t>(rec->ExceptionAddress);
  std::string report = fmt::format("[CRASH] {} (0x{:08X}) on thread {}\n",
                                   ExceptionName(rec->ExceptionCode), rec->ExceptionCode,
                                   GetCurrentThreadId());
  report += fmt::format("  at {}\n", Describe(process, pc));
  if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
       rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0   ? "reading"
                     : rec->ExceptionInformation[0] == 1 ? "writing"
                                                         : "executing";
    const uint64_t target = rec->ExceptionInformation[1];
    report += fmt::format("  {} 0x{:016X}{}\n", op, target, GuestNote(target));
  }

  // Walk the faulting thread's stack. Recompiled functions are named after
  // their guest address (sub_XXXXXXXX), so this reads as the game's call chain.
  report += "  stack:\n";
  CONTEXT ctx = *info->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    report += fmt::format("    #{:<2} {}\n", i, Describe(process, frame.AddrPC.Offset));
  }

  // Minidump for a debugger (open in Visual Studio with the matching .pdb).
  const auto stamp = Stamp();
  const auto dump_path = g_log_dir / fmt::format("amped3_crash_{}.dmp", stamp);
  HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
    // Thread stacks and data near pointers only; the 4 GB guest arena would
    // make a full-memory dump enormous.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpScanMemory | MiniDumpWithThreadInfo);
    if (MiniDumpWriteDump(process, GetCurrentProcessId(), file, type, &mei, nullptr, nullptr)) {
      report += fmt::format("  minidump: {}\n", dump_path.string());
    }
    CloseHandle(file);
  }

  WriteReport("crash", stamp, report);

  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install(const std::filesystem::path& log_dir) {
  g_log_dir = log_dir;
  std::error_code ec;
  std::filesystem::create_directories(g_log_dir, ec);
  g_previous = SetUnhandledExceptionFilter(OnUnhandledException);
  std::atexit(OnExit);
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
  _set_invalid_parameter_handler(OnInvalidParameter);
}

void MarkCleanShutdown() {
  // The user closed the game: exit() is expected, don't report it.
  g_in_handler = true;
}

void WriteHangReport() {
  HANDLE process = GetCurrentProcess();
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();

  struct ThreadStack {
    DWORD id;
    std::string name;
    std::vector<uint64_t> frames;
  };
  std::vector<ThreadStack> threads;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    REXLOG_ERROR("Hang report: could not list threads");
    return;
  }
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  std::vector<DWORD> ids;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
  }
  CloseHandle(snap);
  threads.reserve(ids.size());

  for (DWORD id : ids) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, id);
    if (!h) continue;
    ThreadStack ts{id, {}, {}};
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      char buf[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
      ts.name = buf;
      LocalFree(desc);
    }
    ts.frames.reserve(64);  // no allocation while the thread is paused
    // Pause one thread at a time and only unwind raw addresses while it is
    // paused (no heap, no symbol lookups), so a thread holding a lock can't
    // deadlock the report. Symbols are resolved after it resumes.
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
      CONTEXT ctx = {};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(h, &ctx)) {
        for (int i = 0; i < 64 && ctx.Rip; ++i) {
          ts.frames.push_back(ctx.Rip);
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
          if (fn) {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                             &establisher, nullptr);
          } else {
            // Leaf function: return address is on top of the stack.
            if (!ctx.Rsp) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
          }
        }
      }
      ResumeThread(h);
    }
    CloseHandle(h);
    threads.push_back(std::move(ts));
  }

  InitSymbols(process);
  const auto stamp = Stamp();
  std::string report = fmt::format("[HANG REPORT] {} threads (F9 pressed on thread {})\n",
                                   threads.size(), self);
  report += "Threads whose stacks contain sub_XXXXXXXX frames are running game code;\n"
            "the frames above them show what each is waiting on.\n";
  for (const auto& ts : threads) {
    report += fmt::format("\n=== thread {}{}\n", ts.id, ts.name.empty() ? "" : " \"" + ts.name + "\"");
    for (size_t i = 0; i < ts.frames.size(); ++i) {
      report += fmt::format("    #{:<2} {}\n", i, Describe(process, ts.frames[i]));
    }
  }

  if (g_hang_extra) {
    report += "\n[GUEST WAITS] what each game thread is blocked on\n";
    report += g_hang_extra();
  }

  // Full-thread minidump for Visual Studio.
  const auto dump_path = g_log_dir / fmt::format("amped3_hang_{}.dmp", stamp);
  HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpScanMemory | MiniDumpWithThreadInfo);
    if (MiniDumpWriteDump(process, pid, file, type, nullptr, nullptr, nullptr)) {
      report += fmt::format("\nminidump: {}\n", dump_path.string());
    }
    CloseHandle(file);
  }

  const auto txt_path = g_log_dir / fmt::format("amped3_hang_{}.txt", stamp);
  if (FILE* f = _wfopen(txt_path.c_str(), L"w")) {
    std::fputs(report.c_str(), f);
    std::fclose(f);
  }
  REXLOG_INFO("Hang report written: {} ({} threads)", txt_path.string(), threads.size());
  rex::FlushLogging();
}

void SetHangReportExtra(std::function<std::string()> extra) { g_hang_extra = std::move(extra); }

void SetGuestMemory(const uint8_t* virtual_base, const uint8_t* physical_base) {
  g_vbase = virtual_base;
  g_pbase = physical_base;
}

}  // namespace amped3::crash

#else  // !_WIN32

namespace amped3::crash {
void Install(const std::filesystem::path&) {}
void SetGuestMemory(const uint8_t*, const uint8_t*) {}
void MarkCleanShutdown() {}
void WriteHangReport() {}
void SetHangReportExtra(std::function<std::string()>) {}
}  // namespace amped3::crash

#endif
