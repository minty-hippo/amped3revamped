// amped3 - guest wait diagnostics. See guest_waits.h.

#include "guest_waits.h"

#include <cstdint>
#include <cstring>
#include <map>

#include <fmt/format.h>

#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace amped3::diag {

namespace {

using rex::system::XObject;

const char* TypeName(XObject::Type t) {
  switch (t) {
    case XObject::Type::Enumerator: return "enumerator";
    case XObject::Type::Event: return "event";
    case XObject::Type::File: return "file";
    case XObject::Type::IOCompletion: return "io-completion";
    case XObject::Type::Module: return "module";
    case XObject::Type::Mutant: return "mutant";
    case XObject::Type::NotifyListener: return "notify-listener";
    case XObject::Type::Semaphore: return "semaphore";
    case XObject::Type::Session: return "session";
    case XObject::Type::Socket: return "socket";
    case XObject::Type::SymbolicLink: return "symlink";
    case XObject::Type::Thread: return "thread";
    case XObject::Type::Timer: return "timer";
    default: return "undefined";
  }
}

const char* DispatcherTypeName(uint8_t t) {
  switch (t) {
    case 0: return "notification event";
    case 1: return "synchronization event";
    case 2: return "mutant";
    case 3: return "process";
    case 4: return "queue";
    case 5: return "semaphore";
    case 6: return "thread";
    case 8: return "notification timer";
    case 9: return "synchronization timer";
    default: return "unknown";
  }
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Guest address -> readable host pointer, or nullptr. Guarded so a register
// that only looks like a pointer can't crash the report.
const uint8_t* GuestPtr(rex::memory::Memory* memory, uint32_t addr, size_t size) {
  if (addr < 0x40000000u || addr >= 0xC0000000u) return nullptr;
  auto* host = memory->TranslateVirtual<const uint8_t*>(addr);
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(host, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
      reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < host + size) {
    return nullptr;
  }
#else
  (void)size;
#endif
  return host;
}

}  // namespace

std::string DescribeGuestWaits(rex::system::KernelState* kernel_state) {
  if (!kernel_state) return "  (kernel not running)\n";
  auto* memory = kernel_state->memory();

  // handle -> object, for resolving wait arguments.
  auto objects = kernel_state->object_table()->GetAllObjects();
  std::map<uint32_t, XObject*> by_handle;
  for (auto& obj : objects) {
    for (uint32_t h : obj->handles()) by_handle[h] = obj.get();
  }

  auto describe_handle = [&](uint32_t h) -> std::string {
    auto it = by_handle.find(h);
    if (it == by_handle.end()) return fmt::format("handle {:08X} (not an open handle)", h);
    XObject* obj = it->second;
    std::string out = fmt::format("handle {:08X} = {}", h, TypeName(obj->type()));
    if (!obj->name().empty()) out += fmt::format(" \"{}\"", obj->name());
    if (obj->type() == XObject::Type::Event) {
      uint32_t type = 0, state = 0;
      static_cast<rex::system::XEvent*>(obj)->Query(&type, &state);
      out += fmt::format(" ({}, {})", type ? "auto-reset" : "manual-reset",
                         state ? "SIGNALED" : "not signaled");
    }
    return out;
  };

  auto describe_dispatcher = [&](uint32_t addr) -> std::string {
    const uint8_t* p = GuestPtr(memory, addr, 16);
    if (!p) return {};
    return fmt::format("object @ {:08X} = {} (signal state {})", addr, DispatcherTypeName(p[0]),
                       static_cast<int32_t>(Be32(p + 4)));
  };

  std::string out;
  for (auto& obj : objects) {
    if (obj->type() != XObject::Type::Thread) continue;
    auto* thread = static_cast<rex::system::XThread*>(obj.get());
    if (!thread->is_guest_thread() || !thread->thread_state()) continue;
    const PPCContext* ctx = thread->thread_state()->context();
    if (!ctx) continue;

    const uint32_t r3 = ctx->r3.u32, r4 = ctx->r4.u32, r5 = ctx->r5.u32, r6 = ctx->r6.u32;
    out += fmt::format("\n=== guest thread {:08X}{} (id {})\n", thread->handle(),
                       obj->name().empty() ? "" : " \"" + obj->name() + "\"",
                       thread->thread_id());
    out += fmt::format("    lr {:08X}  r3 {:08X}  r4 {:08X}  r5 {:08X}  r6 {:08X}\n",
                       static_cast<uint32_t>(ctx->lr), r3, r4, r5, r6);

    // Interpret r3/r4 the ways the kernel wait calls use them:
    //   NtWaitForSingleObjectEx(handle, ...)          r3 = handle
    //   NtWaitForMultipleObjectsEx(count, handles*)   r3 = count, r4 = array
    //   KeWaitForSingleObject(object*, ...)           r3 = dispatcher header
    if (r3 >= XObject::kHandleBase && r3 < XObject::kHandleBase + 0x01000000u) {
      out += "    waits on " + describe_handle(r3) + "\n";
    } else if (r3 >= 1 && r3 <= 64) {
      if (const uint8_t* arr = GuestPtr(memory, r4, r3 * 4)) {
        out += fmt::format("    waits on {} handles ({}):\n", r3, r5 ? "any" : "all");
        for (uint32_t i = 0; i < r3; ++i) {
          out += "      " + describe_handle(Be32(arr + i * 4)) + "\n";
        }
      }
    } else {
      std::string d = describe_dispatcher(r3);
      if (!d.empty()) out += "    waits on " + d + "\n";
    }
  }
  return out.empty() ? "  (no guest threads)\n" : out;
}

}  // namespace amped3::diag
