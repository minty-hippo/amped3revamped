// amped3 - the launcher (launcher.h).

#include "launcher.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/logging.h>

#include "graphics_panel.h"
#include "pad_bridge.h"
#include "version.h"

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

REXCVAR_DEFINE_BOOL(show_launcher, true, "Amped 3",
                    "Show the launcher (Play, Update, branch, version) when the game starts. "
                    "Off: the game boots at once; hold LB / View on a controller or Shift while "
                    "it starts to see the launcher anyway.");
REXCVAR_DEFINE_STRING(launcher_branch, "", "Amped 3",
                      "The branch the launcher's Update gets (empty: the branch this build was "
                      "made from).");

namespace amped3 {

namespace xi = rex::input;

namespace {

// The game's menu colors (as the Graphics panel).
const ImU32 kGold = IM_COL32(255, 204, 0, 255);
const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kDim = IM_COL32(255, 255, 255, 120);
const ImU32 kFaint = IM_COL32(255, 255, 255, 60);
const ImU32 kRed = IM_COL32(255, 110, 90, 255);
const ImU32 kGreen = IM_COL32(120, 230, 140, 255);
const ImU32 kBackTop = IM_COL32(6, 10, 26, 255);
const ImU32 kBackBottom = IM_COL32(16, 30, 64, 255);
const ImU32 kPanel = IM_COL32(8, 14, 34, 235);
const ImU32 kEdge = IM_COL32(255, 204, 0, 170);
const ImU32 kSelect = IM_COL32(255, 204, 0, 48);

// Removes terminal color codes (ESC [ ... letter) and other control bytes.
std::string Clean(const std::string& in) {
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

// A child process whose output (stdout and stderr together) arrives line by
// line on a reader thread. The object must outlive the process: owners keep
// it in a shared_ptr the reader thread holds too.
class Child : public std::enable_shared_from_this<Child> {
 public:
  using LineFn = std::function<void(const std::string&)>;
  using ExitFn = std::function<void(int)>;

  bool Start(const std::vector<std::string>& argv, const std::filesystem::path& cwd, LineFn on_line,
             ExitFn on_exit, std::string* error) {
    on_line_ = std::move(on_line);
    on_exit_ = std::move(on_exit);
#if defined(_WIN32)
    std::wstring cmd;
    for (const std::string& a : argv) {
      if (!cmd.empty()) cmd += L' ';
      const std::wstring w = std::filesystem::path(a).wstring();
      // Quoted as CommandLineToArgvW expects.
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
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) {
      *error = "CreatePipe failed";
      return false;
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
    const std::wstring dir = cwd.wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
      CloseHandle(read);
      *error = fmt::format("couldn't start {} (error {})", argv.empty() ? "" : argv[0],
                           GetLastError());
      return false;
    }
    CloseHandle(pi.hThread);
    auto self = shared_from_this();
    std::thread([self, read, process = pi.hProcess] {
      char buf[4096];
      DWORD n = 0;
      while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n > 0) self->Feed(buf, n);
      CloseHandle(read);
      WaitForSingleObject(process, INFINITE);
      DWORD code = 1;
      GetExitCodeProcess(process, &code);
      CloseHandle(process);
      self->Finish(int(code));
    }).detach();
    return true;
#else
    int fds[2];
    if (pipe(fds) != 0) {
      *error = "pipe failed";
      return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    // The game's environment, without libraries preloaded into it (Steam's
    // overlay) and its own library path.
    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e) {
      if (std::strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
      env_store.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (auto& e : env_store) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = argv;
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    // posix_spawn has no working-directory action before glibc 2.29; the
    // scripts don't depend on it (they find their own folder).
    (void)cwd;
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
      close(fds[0]);
      *error = fmt::format("couldn't start {} ({})", argv.empty() ? "" : argv[0], strerror(rc));
      return false;
    }
    auto self = shared_from_this();
    std::thread([self, fd = fds[0], pid] {
      char buf[4096];
      for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
          self->Feed(buf, size_t(n));
          continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
      }
      close(fd);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      self->Finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    }).detach();
    return true;
#endif
  }

 private:
  void Feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const char c = data[i];
      if (c == '\n' || c == '\r') {
        if (!partial_.empty()) on_line_(Clean(partial_));
        partial_.clear();
      } else {
        partial_ += c;
      }
    }
  }
  void Finish(int code) {
    if (!partial_.empty()) on_line_(Clean(partial_));
    partial_.clear();
    on_exit_(code);
  }

  LineFn on_line_;
  ExitFn on_exit_;
  std::string partial_;
};

std::filesystem::path ExeDir() { return rex::filesystem::GetExecutableFolder(); }

// The project folder (sources, update scripts): source.path next to the exe,
// else the exe's build folder's project (out/build/<preset>/ three up).
std::filesystem::path FindSourceDir() {
  const std::filesystem::path exe_dir = ExeDir();
  std::ifstream f(exe_dir / "source.path");
  std::string line;
  if (f && std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) return std::filesystem::path(line);
  }
  std::error_code ec;
  const auto up = exe_dir.parent_path().parent_path().parent_path();
  if (std::filesystem::exists(up / "setup.sh", ec) || std::filesystem::exists(up / "setup.ps1", ec)) {
    return up;
  }
  return {};
}

std::filesystem::path UpdateScript(const std::filesystem::path& source) {
#if defined(_WIN32)
  return source / "update.ps1";
#else
  return source / "update.sh";
#endif
}

std::vector<std::string> ScriptCommand(const std::filesystem::path& source,
                                       const std::vector<std::string>& args) {
#if defined(_WIN32)
  std::vector<std::string> cmd = {"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                   "-File", UpdateScript(source).string()};
  // update.ps1 takes named parameters: action first, then pairs.
  if (!args.empty()) {
    cmd.push_back("-Action");
    cmd.push_back(args[0]);
  }
  if (args.size() > 1) {
    cmd.push_back("-Branch");
    cmd.push_back(args[1]);
  }
  if (args.size() > 2) {
    cmd.push_back(args[0] == "update" ? "-ExeDir" : "-Base");
    cmd.push_back(args[2]);
  }
  return cmd;
#else
  std::vector<std::string> cmd = {"bash", UpdateScript(source).string()};
  cmd.insert(cmd.end(), args.begin(), args.end());
  return cmd;
#endif
}

std::string BuiltBranch() {
  const std::string b = version::Branch();
  return (b == "unknown" || b == "HEAD") ? std::string() : b;
}

}  // namespace

// Shared between the dialog (UI thread) and the scripts' reader threads.
struct LauncherState {
  std::mutex mutex;
  std::filesystem::path source;
  std::string source_problem;  // why Update can't run ("" if it can)

  std::vector<std::string> branches;
  int branch = 0;

  enum class Check { kIdle, kRunning, kDone, kFailed } check = Check::kIdle;
  std::string check_branch;     // the branch the result is for
  std::string remote;           // "sha date subject"
  std::string behind, ahead;    // counts, "?" when unknown
  std::vector<std::string> commits;
  std::string check_error;
  int check_generation = 0;

  enum class Update { kIdle, kRunning, kDone, kFailed } update = Update::kIdle;
  std::string step;
  std::deque<std::string> log;
  std::string update_error;
  std::filesystem::path log_path;
  std::ofstream log_file;

  std::vector<std::shared_ptr<Child>> children;  // kept until they exit
};

bool LauncherWanted() {
  if (REXCVAR_GET(show_launcher)) return true;
#if defined(_WIN32)
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return true;
#endif
  // The controller watcher publishes every few milliseconds; give it a moment
  // to have read the pads once.
  for (int i = 0; i < 12; ++i) {
    const pad::Snapshot s = pad::Latest();
    if (s.buttons & (xi::X_INPUT_GAMEPAD_LEFT_SHOULDER | xi::X_INPUT_GAMEPAD_BACK)) return true;
    if (s.connected) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void CleanUpAfterUpdate(const std::filesystem::path& exe_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(exe_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".old") {
      std::filesystem::remove(entry.path(), ec);
    }
  }
}

Launcher::Launcher(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                   std::function<void()> play, std::function<void()> quit)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      play_(std::move(play)),
      quit_(std::move(quit)),
      state_(std::make_shared<LauncherState>()) {
  auto& st = *state_;
  st.source = FindSourceDir();
  std::error_code ec;
  if (st.source.empty()) {
    st.source_problem =
        "Update needs the project folder this copy was built from, and this copy doesn't say "
        "where it is (no source.path next to the game). Run the installer once more to fix that.";
  } else if (!std::filesystem::exists(UpdateScript(st.source), ec)) {
    st.source_problem = fmt::format("Update needs {}, which isn't there.",
                                    UpdateScript(st.source).string());
  }
  // Branches: the saved choice, this build's, the usual two; GitHub's list
  // replaces them when it arrives.
  std::string want = REXCVAR_GET(launcher_branch);
  if (want.empty()) want = BuiltBranch();
  if (want.empty()) want = "main";
  for (const std::string& b : {std::string("main"), std::string("experimental"), BuiltBranch(), want}) {
    if (!b.empty() && std::find(st.branches.begin(), st.branches.end(), b) == st.branches.end()) {
      st.branches.push_back(b);
    }
  }
  st.branch = int(std::find(st.branches.begin(), st.branches.end(), want) - st.branches.begin());
  REXLOG_INFO("Launcher: version {}, sources {}", version::Text(),
              st.source.empty() ? std::string("unknown") : st.source.string());

  if (st.source_problem.empty()) {
    // The branch list.
    auto state = state_;
    auto child = std::make_shared<Child>();
    auto fetched = std::make_shared<std::vector<std::string>>();
    std::string error;
    if (child->Start(
            ScriptCommand(st.source, {"branches"}), st.source,
            [fetched](const std::string& line) {
              if (line.rfind("::branch:: ", 0) == 0) fetched->push_back(line.substr(11));
            },
            [state, fetched](int code) {
              if (code != 0 || fetched->empty()) return;
              std::lock_guard lock(state->mutex);
              const std::string current = state->branches[size_t(state->branch)];
              std::vector<std::string> list = *fetched;
              if (std::find(list.begin(), list.end(), current) == list.end()) list.push_back(current);
              state->branches = list;
              state->branch =
                  int(std::find(list.begin(), list.end(), current) - list.begin());
            },
            &error)) {
      st.children.push_back(child);
    }
    StartCheck();
  }
}

Launcher::~Launcher() = default;

void Launcher::SaveSettings() {
  if (!config_path_.empty()) rex::cvar::SaveConfig(config_path_);
}

void Launcher::StartCheck() {
  auto state = state_;
  std::lock_guard lock(state->mutex);
  if (!state->source_problem.empty()) return;
  const std::string branch = state->branches[size_t(state->branch)];
  const int generation = ++state->check_generation;
  state->check = LauncherState::Check::kRunning;
  state->check_branch = branch;
  state->remote.clear();
  state->behind.clear();
  state->ahead.clear();
  state->commits.clear();
  state->check_error.clear();
  auto child = std::make_shared<Child>();
  std::string error;
  const bool ok = child->Start(
      ScriptCommand(state->source, {"check", branch, version::Commit()}), state->source,
      [state, generation](const std::string& line) {
        std::lock_guard lock(state->mutex);
        if (generation != state->check_generation) return;
        auto value = [&](const char* tag) -> const char* {
          const size_t n = std::strlen(tag);
          return line.compare(0, n, tag) == 0 ? line.c_str() + n : nullptr;
        };
        if (const char* v = value("::remote:: ")) state->remote = v;
        else if (const char* v = value("::behind:: ")) state->behind = v;
        else if (const char* v = value("::ahead:: ")) state->ahead = v;
        else if (const char* v = value("::commit:: ")) state->commits.push_back(v);
        else if (const char* v = value("::error:: ")) state->check_error = v;
      },
      [state, generation](int code) {
        std::lock_guard lock(state->mutex);
        if (generation != state->check_generation) return;
        state->check = code == 0 ? LauncherState::Check::kDone : LauncherState::Check::kFailed;
        if (code != 0 && state->check_error.empty()) {
          state->check_error = fmt::format("The check failed (exit {}).", code);
        }
      },
      &error);
  if (ok) {
    state->children.push_back(child);
  } else {
    state->check = LauncherState::Check::kFailed;
    state->check_error = error;
  }
}

void Launcher::StartUpdate() {
  auto state = state_;
  std::lock_guard lock(state->mutex);
  if (!state->source_problem.empty() || state->update == LauncherState::Update::kRunning) return;
  const std::string branch = state->branches[size_t(state->branch)];
  state->update = LauncherState::Update::kRunning;
  state->step = "Starting";
  state->log.clear();
  state->update_error.clear();
  // Everything is also written to logs/update_<time>.log next to the exe.
  {
    std::error_code ec;
    std::filesystem::create_directories(ExeDir() / "logs", ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    state->log_path = ExeDir() / "logs" / fmt::format("update_{}.log", stamp);
    state->log_file.open(state->log_path);
  }
  REXLOG_INFO("Launcher: updating to {} ({})", branch, state->log_path.string());
  auto child = std::make_shared<Child>();
  std::string error;
  const bool ok = child->Start(
      ScriptCommand(state->source, {"update", branch, ExeDir().string()}), state->source,
      [state](const std::string& line) {
        std::lock_guard lock(state->mutex);
        if (state->log_file) state->log_file << line << '\n' << std::flush;
        if (line.rfind("::step:: ", 0) == 0) {
          state->step = line.substr(9);
          state->log.push_back("== " + state->step);
        } else if (line.rfind("::error:: ", 0) == 0) {
          state->update_error = line.substr(10);
        } else if (line.rfind("::", 0) == 0) {
          // ::done:: and other markers
        } else if (!line.empty()) {
          state->log.push_back(line);
        }
        while (state->log.size() > 400) state->log.pop_front();
      },
      [state](int code) {
        std::lock_guard lock(state->mutex);
        state->update =
            code == 0 ? LauncherState::Update::kDone : LauncherState::Update::kFailed;
        if (code != 0 && state->update_error.empty()) {
          state->update_error = fmt::format("The update failed (exit {}).", code);
        }
        if (state->log_file) {
          state->log_file << (code == 0 ? "Update finished.\n" : "Update failed.\n");
          state->log_file.close();
        }
        REXLOG_INFO("Launcher: update {} (exit {})", code == 0 ? "finished" : "failed", code);
      },
      &error);
  if (ok) {
    state->children.push_back(child);
  } else {
    state->update = LauncherState::Update::kFailed;
    state->update_error = error;
  }
}

// Starts the (updated) game again with the same arguments, then quits.
void Launcher::Restart() {
#if defined(_WIN32)
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = GetCommandLineW();
  const std::wstring exe = rex::filesystem::GetExecutablePath().wstring();
  const std::wstring dir = ExeDir().wstring();
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    REXLOG_ERROR("Launcher: couldn't start the game again (error {})", GetLastError());
  }
#else
  std::vector<std::string> args;
  {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(f, arg, '\0')) args.push_back(arg);
  }
  // /proc/self/exe follows the file: after an update moved it aside it names
  // amped3.old, and the new one is at the old name.
  std::string exe = rex::filesystem::GetExecutablePath().string();
  for (const char* suffix : {" (deleted)", ".old"}) {
    const size_t n = std::strlen(suffix);
    if (exe.size() > n && exe.compare(exe.size() - n, n, suffix) == 0) exe.resize(exe.size() - n);
  }
  if (args.empty()) args.push_back(exe);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
  if (rc != 0) REXLOG_ERROR("Launcher: couldn't start the game again ({})", strerror(rc));
#endif
  if (quit_) quit_();
}

namespace {
enum Row { kPlay, kRestart, kBranch, kUpdate, kCheck, kShowLauncher, kQuit, kRowCount };
}  // namespace

void Launcher::ChangeBranch(int step) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->update == LauncherState::Update::kRunning) return;
    const int n = int(state_->branches.size());
    if (n <= 1) return;
    state_->branch = (state_->branch + step + n) % n;
    rex::cvar::SetFlagByName("launcher_branch", state_->branches[size_t(state_->branch)]);
    state_->check = LauncherState::Check::kIdle;
  }
  SaveSettings();
  confirm_update_until_ = 0;
  // Checked once the choice settles.
  check_at_ = ImGui::GetTime() + 0.6;
}

void Launcher::Activate(int row) {
  bool updating;
  bool updated;
  {
    std::lock_guard lock(state_->mutex);
    updating = state_->update == LauncherState::Update::kRunning;
    updated = state_->update == LauncherState::Update::kDone;
  }
  switch (row) {
    case kPlay:
      if (updating) return;
      if (play_) {
        auto play = std::move(play_);
        play_ = nullptr;
        Close();
        play();
      }
      break;
    case kRestart:
      if (updated) Restart();
      break;
    case kBranch:
      ChangeBranch(1);
      break;
    case kUpdate: {
      if (updating) return;
      const double now = ImGui::GetTime();
      if (now < confirm_update_until_) {
        confirm_update_until_ = 0;
        StartUpdate();
      } else {
        confirm_update_until_ = now + 6.0;
      }
      break;
    }
    case kCheck:
      if (!updating) StartCheck();
      break;
    case kShowLauncher:
      rex::cvar::SetFlagByName("show_launcher", REXCVAR_GET(show_launcher) ? "false" : "true");
      SaveSettings();
      break;
    case kQuit:
      if (!updating && quit_) quit_();
      break;
    default:
      break;
  }
}

void Launcher::HandleInput() {
  const pad::Snapshot s = pad::Latest();
  constexpr int kDeadzone = 14000;
  const uint16_t b = s.buttons;
  const uint16_t pressed = first_frame_ ? 0 : static_cast<uint16_t>(b & ~prev_buttons_);
  // Buttons held at start (LB / View to open the launcher) aren't input.
  prev_buttons_ = first_frame_ ? 0xFFFF : b;

  int direction = 0;
  if ((b & xi::X_INPUT_GAMEPAD_DPAD_UP) || s.ly > kDeadzone) direction = -1;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_DOWN) || s.ly < -kDeadzone) direction = 1;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_LEFT) || s.lx < -kDeadzone) direction = -2;
  else if ((b & xi::X_INPUT_GAMEPAD_DPAD_RIGHT) || s.lx > kDeadzone) direction = 2;
  if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) direction = -1;
  else if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) direction = 1;
  else if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) direction = -2;
  else if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) direction = 2;

  const double now = ImGui::GetTime();
  bool fire = false;
  if (direction != held_direction_) {
    held_direction_ = direction;
    fire = direction != 0;
    repeat_at_ = now + 0.35;
  } else if (direction != 0 && now >= repeat_at_) {
    fire = direction == -1 || direction == 1;  // only up / down repeat
    repeat_at_ = now + 0.12;
  }
  bool restart_row;
  {
    std::lock_guard lock(state_->mutex);
    restart_row = state_->update == LauncherState::Update::kDone;
  }
  auto visible = [&](int row) { return row != kRestart || restart_row; };
  if (fire && (direction == -1 || direction == 1)) {
    int row = cursor_;
    do {
      row = (row + direction + kRowCount) % kRowCount;
    } while (!visible(row));
    cursor_ = row;
    confirm_update_until_ = 0;
  }
  if (fire && (direction == -2 || direction == 2) && cursor_ == kBranch) {
    ChangeBranch(direction > 0 ? 1 : -1);
  }
  if ((pressed & xi::X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    Activate(cursor_);
  }
  if (!visible(cursor_)) cursor_ = kPlay;
}

void Launcher::OnDraw(ImGuiIO& io) {
  // The controller drives ImGui's navigation in the other menus; here our own
  // rows do.
  HandleInput();
  const bool first = first_frame_;
  first_frame_ = false;
  if (check_at_ > 0 && ImGui::GetTime() >= check_at_) {
    check_at_ = 0;
    StartCheck();
  }

  // A copy of what the scripts reported, so drawing holds no lock.
  std::string source_problem, remote, behind, ahead, check_error, check_branch, step,
      update_error, log_path;
  std::vector<std::string> commits, log_tail, branches;
  int branch = 0;
  LauncherState::Check check;
  LauncherState::Update update;
  {
    std::lock_guard lock(state_->mutex);
    source_problem = state_->source_problem;
    remote = state_->remote;
    behind = state_->behind;
    ahead = state_->ahead;
    commits = state_->commits;
    check_error = state_->check_error;
    check_branch = state_->check_branch;
    check = state_->check;
    update = state_->update;
    step = state_->step;
    update_error = state_->update_error;
    log_path = state_->log_path.string();
    branches = state_->branches;
    branch = state_->branch;
    const size_t keep = 14;
    const size_t from = state_->log.size() > keep ? state_->log.size() - keep : 0;
    for (size_t i = from; i < state_->log.size(); ++i) log_tail.push_back(state_->log[i]);
    // Children that have exited can go.
  }
  const bool updating = update == LauncherState::Update::kRunning;
  const std::string branch_name = branches.empty() ? std::string() : branches[size_t(branch)];
  const bool same_branch = branch_name == BuiltBranch();

  const ImVec2 display = io.DisplaySize;
  const float unit = std::max(display.y / 720.0f, 0.5f);
  const float text_px = 19.0f * unit;
  const float small_px = 15.0f * unit;
  const float title_px = 40.0f * unit;
  ImFont* text_font = UiFont(text_px);
  ImFont* small_font = UiFont(small_px);
  ImFont* title_font = UiFont(title_px);

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  if (first) ImGui::SetNextWindowFocus();
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (!ImGui::Begin("###amped3_launcher", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackTop, kBackTop, kBackBottom, kBackBottom);

  const float pad = 22.0f * unit;
  const float width = std::min(display.x - 2 * pad, 820.0f * unit);
  const float x0 = std::round((display.x - width) * 0.5f);
  float y = std::round(display.y * 0.07f);

  // Title and version.
  dl->AddText(title_font, title_px, ImVec2(x0, y), kGold, "AMPED 3 REVAMPED");
  y += title_px * 1.15f;
  const std::string version_line = fmt::format("Version  {}", version::Text());
  dl->AddText(small_font, small_px, ImVec2(x0, y), kDim, version_line.c_str());
  y += small_px * 2.0f;

  // The rows.
  const float row_h = text_px * 1.75f;
  const float value_x = x0 + width * 0.42f;
  struct RowText {
    int row;
    std::string label, value;
    ImU32 value_color = kWhite;
    bool enabled = true;
  };
  std::vector<RowText> rows;
  rows.push_back({kPlay, "Play", update == LauncherState::Update::kDone ? "this version" : "",
                  kDim, !updating});
  if (update == LauncherState::Update::kDone) {
    rows.push_back({kRestart, "Restart", "play the updated game", kGreen});
  }
  rows.push_back({kBranch, "Update branch", fmt::format("<  {}  >", branch_name), kWhite,
                  !updating});
  {
    RowText r{kUpdate, "Update", "", kWhite, source_problem.empty() && !updating};
    if (!source_problem.empty()) {
      r.value = "unavailable";
      r.value_color = kDim;
    } else if (updating) {
      r.value = "updating...";
      r.value_color = kGold;
    } else if (ImGui::GetTime() < confirm_update_until_) {
      r.value = "press again to start";
      r.value_color = kGold;
    } else if (check == LauncherState::Check::kRunning) {
      r.value = "checking...";
      r.value_color = kDim;
    } else if (check == LauncherState::Check::kDone && check_branch == branch_name) {
      if (!same_branch) {
        r.value = fmt::format("switch to {}", branch_name);
        r.value_color = kGold;
      } else if (behind == "0") {
        r.value = "up to date";
        r.value_color = kGreen;
      } else if (!behind.empty() && behind != "?") {
        r.value = fmt::format("{} new change{}", behind, behind == "1" ? "" : "s");
        r.value_color = kGold;
      } else {
        r.value = "rebuild from GitHub";
        r.value_color = kDim;
      }
    } else if (check == LauncherState::Check::kFailed) {
      r.value = "couldn't check";
      r.value_color = kRed;
    }
    rows.push_back(r);
  }
  rows.push_back({kCheck, "Check for updates", "", kWhite, source_problem.empty() && !updating});
  rows.push_back({kShowLauncher, "Show launcher at start",
                  REXCVAR_GET(show_launcher) ? "On" : "Off", kWhite});
  rows.push_back({kQuit, "Quit", "", kWhite, !updating});

  const float panel_top = y;
  const float rows_h = row_h * float(rows.size());
  dl->AddRectFilled(ImVec2(x0 - pad * 0.6f, panel_top - pad * 0.5f),
                    ImVec2(x0 + width + pad * 0.6f, panel_top + rows_h + pad * 0.5f), kPanel,
                    10.0f * unit);
  dl->AddRect(ImVec2(x0 - pad * 0.6f, panel_top - pad * 0.5f),
              ImVec2(x0 + width + pad * 0.6f, panel_top + rows_h + pad * 0.5f), kEdge,
              10.0f * unit, 0, 1.5f * unit);
  for (const RowText& r : rows) {
    const ImVec2 a(x0 - pad * 0.3f, y);
    const ImVec2 b(x0 + width + pad * 0.3f, y + row_h);
    // Mouse: hovering moves the selection, a click activates.
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(r.row);
    ImGui::InvisibleButton("row", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
      cursor_ = r.row;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      cursor_ = r.row;
      // The branch: left half goes back, right half forward.
      if (r.row == kBranch) ChangeBranch(io.MousePos.x < value_x + width * 0.15f ? -1 : 1);
      else Activate(r.row);
    }
    ImGui::PopID();
    const bool selected = cursor_ == r.row;
    if (selected) dl->AddRectFilled(a, b, kSelect, 6.0f * unit);
    const float ty = y + (row_h - text_px) * 0.5f;
    const ImU32 label_color = !r.enabled ? kFaint : selected ? kGold : kWhite;
    dl->AddText(text_font, text_px, ImVec2(x0, ty), label_color, r.label.c_str());
    if (!r.value.empty()) {
      dl->AddText(text_font, text_px, ImVec2(value_x, ty), r.enabled ? r.value_color : kFaint,
                  r.value.c_str());
    }
    y += row_h;
  }
  y += pad * 1.2f;

  // Below: the update's output, or what's new on the branch.
  const float line_h = small_px * 1.35f;
  const float bottom = display.y - small_px * 3.0f;
  auto line = [&](const std::string& text, ImU32 color) {
    if (y + line_h > bottom) return;
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x0 + width, y + line_h), true);
    dl->AddText(small_font, small_px, ImVec2(x0, y), color, text.c_str());
    dl->PopClipRect();
    y += line_h;
  };
  if (update != LauncherState::Update::kIdle) {
    if (updating) line(fmt::format("Updating to {}: {}", branch_name, step), kGold);
    if (update == LauncherState::Update::kDone) {
      line("Update finished. Restart to play the new version.", kGreen);
    }
    if (update == LauncherState::Update::kFailed) {
      line(update_error.empty() ? std::string("The update failed.") : update_error, kRed);
      line(fmt::format("Full output: {}", log_path), kDim);
    }
    y += line_h * 0.3f;
    // As many of the last lines as fit.
    const int fit = std::max(0, int((bottom - y) / line_h));
    const size_t from = log_tail.size() > size_t(fit) ? log_tail.size() - size_t(fit) : 0;
    for (size_t i = from; i < log_tail.size(); ++i) line(log_tail[i], kDim);
  } else if (!source_problem.empty()) {
    // Wrapped by hand: a few short lines.
    std::string rest = source_problem;
    const float max_w = width;
    while (!rest.empty()) {
      size_t cut = rest.size();
      while (cut > 0 &&
             small_font->CalcTextSizeA(small_px, FLT_MAX, 0, rest.c_str(), rest.c_str() + cut).x >
                 max_w) {
        cut = rest.rfind(' ', cut - 1);
        if (cut == std::string::npos) {
          cut = rest.size();
          break;
        }
      }
      line(rest.substr(0, cut), kDim);
      rest = cut < rest.size() ? rest.substr(cut + 1) : std::string();
    }
  } else if (check == LauncherState::Check::kFailed) {
    line(check_error.empty() ? std::string("Couldn't reach GitHub.") : check_error, kRed);
  } else if (check == LauncherState::Check::kDone && check_branch == branch_name) {
    if (!remote.empty()) {
      // "sha date subject"
      const size_t s1 = remote.find(' ');
      const size_t s2 = s1 == std::string::npos ? s1 : remote.find(' ', s1 + 1);
      const std::string sha = remote.substr(0, std::min<size_t>(7, remote.size()));
      const std::string date = s2 == std::string::npos ? "" : remote.substr(s1 + 1, s2 - s1 - 1);
      line(fmt::format("Latest on {}: {} ({})", branch_name, sha, date), kWhite);
    }
    if (!same_branch && !ahead.empty() && ahead != "0" && ahead != "?") {
      line(fmt::format("This build has {} change{} that {} doesn't.", ahead, ahead == "1" ? "" : "s",
                       branch_name),
           kDim);
    }
    if (!commits.empty()) {
      line(same_branch ? "New since this build:" : fmt::format("Newest on {}:", branch_name), kDim);
      for (const std::string& c : commits) line("  " + c, kWhite);
    }
  }

  // Controls.
  const char* hint =
      "A / Enter: select     Left / Right: branch     Hold LB / View or Shift while the game "
      "starts to see this when it's off";
  dl->AddText(small_font, small_px * 0.9f, ImVec2(x0, display.y - small_px * 2.0f), kFaint, hint);

  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace amped3
