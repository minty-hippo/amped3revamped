// amped3 - what a thread is doing, for the crash report.
//
// A driver can crash inside a call we make (a pipeline its compiler can't
// handle, say), where the stack shows only the driver's own frames. The
// renderer sets a note on the thread before such a call and clears it after;
// the crash report (crash_report.cpp) prints the faulting thread's note and,
// when a path is set, appends it to that file, so the next start can avoid
// the same thing (renderer.cpp: crashed_pipelines.txt).
//
// Header-only (the replay tool links the renderer without the reporter).

#pragma once

#include <cstddef>
#include <cstring>

namespace amped3::crash_note {

inline thread_local char g_note[512] = {};
inline char g_note_path[1024] = {};  // where a crash appends the note (empty: nowhere)

inline void Set(const char* text) {
  std::strncpy(g_note, text, sizeof(g_note) - 1);
  g_note[sizeof(g_note) - 1] = 0;
}
inline void Clear() { g_note[0] = 0; }
inline void SetPath(const char* path) {
  std::strncpy(g_note_path, path, sizeof(g_note_path) - 1);
  g_note_path[sizeof(g_note_path) - 1] = 0;
}

// Scoped: set for the call, cleared after.
struct Scope {
  explicit Scope(const char* text) { Set(text); }
  ~Scope() { Clear(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace amped3::crash_note
