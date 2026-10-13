// amped3-launcher - child processes (process.h).

#include "process.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace launcher {

namespace {

std::mutex g_running_mutex;
#if defined(_WIN32)
HANDLE g_running_job = nullptr;
#else
pid_t g_running_pid = 0;
#endif

#if defined(_WIN32)
std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::wstring CommandLine(const std::vector<std::string>& argv) {
  std::wstring cmd;
  for (const std::string& a : argv) {
    if (!cmd.empty()) cmd += L' ';
    const std::wstring w = Widen(a);
    cmd += L'"';
    size_t backslashes = 0;
    for (wchar_t c : w) {
      if (c == L'\\') {
        ++backslashes;
      } else if (c == L'"') {
        cmd.append(backslashes * 2 + 1, L'\\');
        backslashes = 0;
      } else {
        backslashes = 0;
      }
      cmd += c;
    }
    cmd.append(backslashes, L'\\');
    cmd += L'"';
  }
  return cmd;
}
#else
// This process's environment without libraries preloaded into it (Steam's
// overlay): the scripts and tools don't need them.
std::vector<std::string> ChildEnvironment() {
  std::vector<std::string> env;
  for (char** e = environ; e && *e; ++e) {
    if (std::strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
    env.emplace_back(*e);
  }
  return env;
}
#endif

}  // namespace

std::string CleanLine(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == 0x1B) {
      if (i + 1 < in.size() && in[i + 1] == '[') {
        i += 2;
        while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;
      }
      continue;
    }
    if (c == '\t') {
      out += "    ";
      continue;
    }
    if (c < 0x20) continue;
    out += static_cast<char>(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

int RunProcess(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
               const std::function<void(const std::string&)>& on_line, std::string* error) {
  std::string partial;
  auto feed = [&](const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const char c = data[i];
      if (c == '\n' || c == '\r') {
        if (!partial.empty() && on_line) on_line(CleanLine(partial));
        partial.clear();
      } else {
        partial += c;
      }
    }
  };
#if defined(_WIN32)
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &sa, 0)) {
    if (error) *error = "CreatePipe failed";
    return -1;
  }
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = null_in;
  si.hStdOutput = write;
  si.hStdError = write;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = CommandLine(argv);
  const std::wstring dir = cwd.wstring();
  // In a job, so cancelling (or closing the launcher) ends the whole tree.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                 dir.empty() ? nullptr : dir.c_str(), &si, &pi);
  CloseHandle(write);
  if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
  if (!ok) {
    const DWORD e = GetLastError();
    CloseHandle(read);
    if (job) CloseHandle(job);
    if (error) *error = "couldn't start " + (argv.empty() ? std::string() : argv[0]) +
                        " (error " + std::to_string(e) + ")";
    return -1;
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  {
    std::lock_guard lock(g_running_mutex);
    g_running_job = job;
  }
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n > 0) feed(buf, n);
  CloseHandle(read);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  {
    std::lock_guard lock(g_running_mutex);
    g_running_job = nullptr;
  }
  if (job) CloseHandle(job);
  if (!partial.empty() && on_line) on_line(CleanLine(partial));
  return int(code);
#else
  int fds[2];
  if (pipe(fds) != 0) {
    if (error) *error = "pipe failed";
    return -1;
  }
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  // Its own process group, so cancelling stops everything it started.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  std::vector<std::string> env_store = ChildEnvironment();
  std::vector<char*> envp;
  for (auto& e : env_store) envp.push_back(e.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = argv;
  std::vector<char*> cargv;
  for (auto& a : args) cargv.push_back(a.data());
  cargv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, cargv[0], &actions, &attr, cargv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    if (error) *error = "couldn't start " + argv[0] + " (" + std::strerror(rc) + ")";
    return -1;
  }
  {
    std::lock_guard lock(g_running_mutex);
    g_running_pid = pid;
  }
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n > 0) {
      feed(buf, size_t(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  {
    std::lock_guard lock(g_running_mutex);
    g_running_pid = 0;
  }
  if (!partial.empty() && on_line) on_line(CleanLine(partial));
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

int RunCapture(const std::vector<std::string>& argv, std::string* output, std::string* error) {
  output->clear();
  return RunProcess(
      argv, {}, [&](const std::string& line) { *output += line + "\n"; }, error);
}

void CancelRunning() {
  std::lock_guard lock(g_running_mutex);
#if defined(_WIN32)
  if (g_running_job) TerminateJobObject(g_running_job, 1);
#else
  if (g_running_pid > 0) {
    const pid_t group = g_running_pid;
    kill(-group, SIGTERM);
    // Anything still there a few seconds later is stopped hard.
    std::thread([group] {
      std::this_thread::sleep_for(std::chrono::seconds(4));
      kill(-group, SIGKILL);
    }).detach();
  }
#endif
}

bool StartDetached(const std::vector<std::string>& argv, const std::filesystem::path& cwd) {
#if defined(_WIN32)
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = CommandLine(argv);
  const std::wstring dir = cwd.wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
  std::vector<std::string> args = argv;
  std::vector<char*> cargv;
  for (auto& a : args) cargv.push_back(a.data());
  cargv.push_back(nullptr);
  std::vector<std::string> env_store = ChildEnvironment();
  std::vector<char*> envp;
  for (auto& e : env_store) envp.push_back(e.data());
  envp.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  return rc == 0;
#endif
}

bool RunGame(const std::filesystem::path& exe, const std::filesystem::path& cwd,
             std::string* error) {
#if defined(_WIN32)
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  const std::wstring dir = cwd.wstring();
  if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir.c_str(), &si, &pi)) {
    if (error) *error = "couldn't start the game (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  // Steam and other front ends see the game as running while we wait.
  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess);
  ExitProcess(0);
#else
  if (chdir(cwd.c_str()) != 0) {
    if (error) *error = "couldn't enter " + cwd.string();
    return false;
  }
  const std::string path = exe.string();
  char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
  execv(path.c_str(), argv);
  if (error) *error = "couldn't start the game (" + std::string(std::strerror(errno)) + ")";
  return false;
#endif
}

}  // namespace launcher
