// amped3 - scheduling hints for hybrid CPUs. See thread_qos.h.

#include "thread_qos.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace amped3::qos {

#if defined(_WIN32)
namespace {

// The power throttling state structures and information classes (Windows 10
// 1709+), declared here so older SDK headers build too.
struct PowerThrottlingState {
  ULONG Version;
  ULONG ControlMask;
  ULONG StateMask;
};
constexpr ULONG kPowerThrottlingVersion = 1;
constexpr ULONG kExecutionSpeed = 0x1;
constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS
constexpr int kThreadPowerThrottling = 3;   // THREAD_INFORMATION_CLASS

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

FARPROC Kernel32(const char* name) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  return k ? GetProcAddress(k, name) : nullptr;
}

}  // namespace

void PreferPerformanceProcess() {
  auto set = reinterpret_cast<SetProcessInformationFn>(Kernel32("SetProcessInformation"));
  if (!set) return;
  // ControlMask set, StateMask clear: execution speed never throttled.
  PowerThrottlingState st{kPowerThrottlingVersion, kExecutionSpeed, 0};
  set(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st));
}

void PreferPerformanceThread(const wchar_t* name, bool raise_priority) {
  if (auto set = reinterpret_cast<SetThreadInformationFn>(Kernel32("SetThreadInformation"))) {
    PowerThrottlingState st{kPowerThrottlingVersion, kExecutionSpeed, 0};
    set(GetCurrentThread(), kThreadPowerThrottling, &st, sizeof(st));
  }
  if (raise_priority) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  if (name) {
    if (auto desc = reinterpret_cast<SetThreadDescriptionFn>(Kernel32("SetThreadDescription"))) {
      desc(GetCurrentThread(), name);
    }
  }
}
#else
void PreferPerformanceProcess() {}
void PreferPerformanceThread(const wchar_t*, bool) {}
#endif

}  // namespace amped3::qos
