// amped3-launcher - screens and jobs (app.h).

#include "app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "json.h"
#include "process.h"

namespace launcher {

namespace {

const ImU32 kGold = IM_COL32(255, 204, 0, 255);

// Real-ESRGAN's ready-made builds: Windows and Linux x86_64 (not ARM).
#if defined(__aarch64__) && !defined(_WIN32)
constexpr bool kCanUpscale = false;
#else
constexpr bool kCanUpscale = true;
#endif

const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kDim = IM_COL32(255, 255, 255, 130);
const ImU32 kFaint = IM_COL32(255, 255, 255, 60);
const ImU32 kRed = IM_COL32(255, 110, 90, 255);
const ImU32 kGreen = IM_COL32(120, 230, 140, 255);
const ImU32 kBackTop = IM_COL32(6, 10, 26, 255);
const ImU32 kBackBottom = IM_COL32(16, 30, 64, 255);
const ImU32 kPanel = IM_COL32(8, 14, 34, 235);
const ImU32 kEdge = IM_COL32(255, 204, 0, 170);
const ImU32 kSelect = IM_COL32(255, 204, 0, 48);

constexpr uint64_t kNeededBytes = 40ull << 30;

#if defined(_WIN32)
const char* kCurl = "curl.exe";
const char* kTar = "tar.exe";
#else
const char* kCurl = "curl";
const char* kTar = "tar";
#endif

std::string Shorten(const std::string& s, size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

std::string FirstLine(const std::string& s) {
  const size_t nl = s.find('\n');
  std::string line = nl == std::string::npos ? s : s.substr(0, nl);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return line;
}

std::string GiB(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f GB", double(bytes) / double(1ull << 30));
  return buf;
}

std::string Elapsed(std::chrono::steady_clock::duration d) {
  const auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
  char buf[32];
  if (s >= 3600) std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
  else std::snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
  return buf;
}

// GET https://api.github.com/repos/<repo><path>.
bool GitHub(const std::string& repo, const std::string& path, Json* out, std::string* error) {
  std::string body, err;
  const int code = RunCapture({kCurl, "-fsSL", "--max-time", "20", "-H",
                               "User-Agent: amped3-revamped-launcher", "-H",
                               "Accept: application/vnd.github+json",
                               "https://api.github.com/repos/" + repo + path},
                              &body, &err);
  if (code != 0) {
    *error = code < 0 ? err : "Couldn't reach GitHub (" + FirstLine(body) + ")";
    return false;
  }
  bool ok = false;
  *out = Json::Parse(body, &ok);
  if (!ok) *error = "Unexpected reply from GitHub.";
  return ok;
}

struct CommitInfo {
  std::string sha, date, subject;
};

bool BranchHead(const std::string& repo, const std::string& branch, CommitInfo* info,
                std::string* error) {
  Json j;
  if (!GitHub(repo, "/commits/" + branch, &j, error)) return false;
  info->sha = j["sha"].str();
  info->date = Shorten(j["commit"]["committer"]["date"].str(), 10);
  info->subject = FirstLine(j["commit"]["message"].str());
  if (info->sha.empty()) {
    *error = "GitHub doesn't know the branch " + branch + ".";
    return false;
  }
  return true;
}

// Version files: branch=, commit=, date=.
struct Version {
  std::string branch, commit, date;
};

Version ReadVersion(const fs::path& file) {
  const auto kv = ReadKeyValues(file);
  Version v;
  if (auto it = kv.find("branch"); it != kv.end()) v.branch = it->second;
  if (auto it = kv.find("commit"); it != kv.end()) v.commit = it->second;
  if (auto it = kv.find("date"); it != kv.end()) v.date = it->second;
  return v;
}

bool FileContains(const fs::path& file, const std::string& text) {
  std::ifstream f(file, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str().find(text) != std::string::npos;
}

bool CopyTree(const fs::path& from, const fs::path& to, std::string* error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) {
    *error = "Copying the sources failed: " + ec.message();
    return false;
  }
  return true;
}

void Wrap(ImFont* font, float px, const std::string& text, float width,
          std::vector<std::string>* lines) {
  std::string rest = text;
  while (!rest.empty()) {
    size_t cut = rest.size();
    while (cut > 0 && font->CalcTextSizeA(px, FLT_MAX, 0, rest.c_str(), rest.c_str() + cut).x > width) {
      const size_t sp = rest.rfind(' ', cut - 1);
      if (sp == std::string::npos || sp == 0) break;
      cut = sp;
    }
    lines->push_back(rest.substr(0, cut));
    rest = cut < rest.size() ? rest.substr(cut + (rest[cut] == ' ' ? 1 : 0)) : std::string();
  }
}

}  // namespace

struct App::Shared {
  std::mutex m;
  std::vector<std::string> branches = {"main", "experimental"};
  int branch = 0;

  enum class Check { kIdle, kRunning, kDone, kFailed } check = Check::kIdle;
  int check_gen = 0;
  std::string check_branch;
  CommitInfo remote;
  int behind = -1, ahead = -1;
  std::vector<std::string> commits;
  std::string check_error;

  enum class Job { kIdle, kRunning, kDone, kFailed, kCancelled } job = Job::kIdle;
  bool job_install = false;
  bool job_upscale = false;  // the texture upscaler on its own
  std::string step, job_error;
  std::deque<std::string> log;
  fs::path log_path;
  std::ofstream log_file;
  std::chrono::steady_clock::time_point job_start, job_end;
  std::atomic<bool> cancel{false};

  // File dialogs answer on their own thread.
  std::string picked_disc, picked_folder;
  std::string dialog_error;

  void Log(const std::string& line) {
    std::lock_guard lock(m);
    if (log_file) log_file << line << '\n' << std::flush;
    log.push_back(line);
    while (log.size() > 500) log.pop_front();
  }
  void Step(const std::string& text) {
    {
      std::lock_guard lock(m);
      step = text;
    }
    Log("== " + text);
  }
};

App::App() : s_(std::make_shared<Shared>()) {
  LoadSettings();
  discs_ = FindDiscImages();
  StartBranchList();
  if (IsInstalled(install_dir_)) {
    // A launcher run from elsewhere (a new download) takes the installed
    // one's place.
    RelocateLauncher(install_dir_);
    StartCheck();
  }
}

App::~App() {
  if (Busy()) CancelRunning();
}

bool App::Busy() const {
  std::lock_guard lock(s_->m);
  return s_->job == Shared::Job::kRunning;
}

void App::LoadSettings() {
  const auto kv = ReadKeyValues(ConfigDir() / "launcher.ini");
  auto get = [&](const char* k) -> std::string {
    auto it = kv.find(k);
    return it == kv.end() ? std::string() : it->second;
  };
  install_dir_ = get("install_dir").empty() ? DefaultInstallDir() : PathFrom(get("install_dir"));
  // The sources: where the install says it was built from, else beside it.
  source_dir_ = PathFrom(get("source_dir"));
  if (source_dir_.empty()) {
    const auto sp = install_dir_ / "source.path";
    std::ifstream f(sp);
    std::string line;
    if (f && std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      source_dir_ = PathFrom(line);
    }
  }
  if (source_dir_.empty()) source_dir_ = install_dir_ / "build" / "amped3revamped";
  if (!get("repo").empty()) repo_ = get("repo");
  direct_start_ = get("direct_start") == "1";
  original_look_ = get("look") == "original";
  upscale_on_install_ = get("upscale_on_install") == "1";
  std::string branch = get("branch");
  if (branch.empty()) {
    const Version v = ReadVersion(install_dir_ / "amped3-version.txt");
    branch = v.branch.empty() ? "main" : v.branch;
  }
  std::lock_guard lock(s_->m);
  auto& b = s_->branches;
  if (std::find(b.begin(), b.end(), branch) == b.end()) b.push_back(branch);
  s_->branch = int(std::find(b.begin(), b.end(), branch) - b.begin());
  const std::string iso = get("iso");
  (void)iso;
}

void App::SaveSettings() {
  std::map<std::string, std::string> kv;
  kv["install_dir"] = PathText(install_dir_);
  kv["source_dir"] = PathText(source_dir_);
  kv["repo"] = repo_;
  kv["direct_start"] = direct_start_ ? "1" : "0";
  kv["look"] = original_look_ ? "original" : "revamped";
  kv["upscale_on_install"] = upscale_on_install_ ? "1" : "0";
  {
    std::lock_guard lock(s_->m);
    kv["branch"] = s_->branches[size_t(s_->branch)];
  }
  if (!discs_.empty()) kv["iso"] = PathText(discs_[size_t(disc_)]);
  WriteKeyValues(ConfigDir() / "launcher.ini", kv);
}

bool App::WantsDirectStart() const {
  if (!direct_start_ || !IsInstalled(install_dir_) || ShiftHeld()) return false;
  // A controller's LB / View held as it starts.
  bool held = false;
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
        SDL_UpdateGamepads();
        held = held || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
               SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK);
        SDL_CloseGamepad(g);
      }
    }
    SDL_free(ids);
  }
  const SDL_Keymod mod = SDL_GetModState();
  return !held && !(mod & SDL_KMOD_SHIFT);
}

void App::Play() {
  std::string error;
  if (window_) SDL_HideWindow(window_);
  if (!RunGame(GameExe(install_dir_), install_dir_, &error)) {
    notice_ = error;
    if (window_) SDL_ShowWindow(window_);
  }
}

std::string App::VersionText() const {
  Version v = ReadVersion(install_dir_ / "amped3-version.txt");
  if (v.commit.empty()) v = ReadVersion(source_dir_ / ".amped3-source");
  if (v.commit.empty()) return "unknown";
  std::string s = v.branch.empty() ? std::string("?") : v.branch;
  s += " " + Shorten(v.commit, 7);
  if (!v.date.empty()) s += " (" + v.date + ")";
  return s;
}

void App::StartBranchList() {
  auto s = s_;
  const std::string repo = repo_;
  std::thread([s, repo] {
    Json j;
    std::string error;
    if (!GitHub(repo, "/branches?per_page=100", &j, &error) || !j.is_array()) return;
    std::vector<std::string> list;
    for (const Json& b : j.items()) {
      if (!b["name"].str().empty()) list.push_back(b["name"].str());
    }
    if (list.empty()) return;
    std::lock_guard lock(s->m);
    const std::string current = s->branches[size_t(s->branch)];
    if (std::find(list.begin(), list.end(), current) == list.end()) list.push_back(current);
    s->branches = list;
    s->branch = int(std::find(list.begin(), list.end(), current) - list.begin());
  }).detach();
}

void App::StartCheck() {
  auto s = s_;
  const std::string repo = repo_;
  const Version installed = ReadVersion(install_dir_ / "amped3-version.txt");
  std::string branch;
  int gen;
  {
    std::lock_guard lock(s->m);
    branch = s->branches[size_t(s->branch)];
    gen = ++s->check_gen;
    s->check = Shared::Check::kRunning;
    s->check_branch = branch;
    s->remote = {};
    s->behind = s->ahead = -1;
    s->commits.clear();
    s->check_error.clear();
  }
  std::thread([s, repo, branch, gen, installed] {
    CommitInfo head;
    std::string error;
    const bool ok = BranchHead(repo, branch, &head, &error);
    int behind = -1, ahead = -1;
    std::vector<std::string> commits;
    if (ok && !installed.commit.empty()) {
      if (head.sha == installed.commit) {
        behind = ahead = 0;
      } else {
        Json c;
        std::string e2;
        if (GitHub(repo, "/compare/" + installed.commit + "..." + head.sha, &c, &e2)) {
          behind = int(c["ahead_by"].num());
          ahead = int(c["behind_by"].num());
          const auto& list = c["commits"].items();
          for (size_t i = list.size(); i-- > 0 && commits.size() < 20;) {
            commits.push_back(Shorten(list[i]["sha"].str(), 7) + "  " +
                              FirstLine(list[i]["commit"]["message"].str()));
          }
        }
      }
    }
    std::lock_guard lock(s->m);
    if (gen != s->check_gen) return;
    s->check = ok ? Shared::Check::kDone : Shared::Check::kFailed;
    s->remote = head;
    s->behind = behind;
    s->ahead = ahead;
    s->commits = commits;
    s->check_error = error;
  }).detach();
}

// Downloads the branch into the sources folder (or updates a git checkout),
// then runs the installer: an install from the disc image, or a rebuild of
// the existing install.
void App::StartJob(bool install, bool upscale) {
  std::string branch;
  {
    std::lock_guard lock(s_->m);
    if (s_->job == Shared::Job::kRunning) return;
    branch = s_->branches[size_t(s_->branch)];
    s_->job = Shared::Job::kRunning;
    s_->job_install = install;
    s_->job_upscale = upscale;
    s_->step = "Starting";
    s_->job_error.clear();
    s_->log.clear();
    s_->cancel = false;
    s_->job_start = std::chrono::steady_clock::now();
    // Everything is also written to a log.
    std::error_code ec;
    const fs::path logs = ConfigDir() / "logs";
    fs::create_directories(logs, ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    s_->log_path =
        logs / (std::string(upscale ? "upscale_" : install ? "install_" : "update_") + stamp + ".log");
    s_->log_file.close();
    s_->log_file.open(s_->log_path);
  }
  SaveSettings();
  const fs::path iso = install && !discs_.empty() ? discs_[size_t(disc_)] : fs::path();
  if (upscale) {
    std::thread([this] { UpscaleMain(); }).detach();
    return;
  }
  std::thread([this, install, branch, iso] { JobMain(install, branch, iso); }).detach();
}

void App::JobMain(bool install, std::string branch, fs::path iso) {
  auto s = s_;
  const fs::path install_dir = install_dir_;
  const fs::path source = source_dir_;
  const std::string repo = repo_;
  auto finish = [&](Shared::Job result, const std::string& error) {
    std::lock_guard lock(s->m);
    s->job = s->cancel ? Shared::Job::kCancelled : result;
    s->job_error = s->cancel ? std::string("Stopped.") : error;
    s->job_end = std::chrono::steady_clock::now();
    if (s->log_file) {
      s->log_file << (result == Shared::Job::kDone ? "Finished.\n" : "Failed: " + error + "\n");
      s->log_file.close();
    }
  };
  auto run = [&](const std::vector<std::string>& argv, const fs::path& cwd) {
    std::string error;
    const int code = RunProcess(argv, cwd, [&](const std::string& line) {
      if (line.rfind("==> ", 0) == 0) s->Step(line.substr(4));
      else s->Log(line);
    }, &error);
    if (code < 0) s->Log(error);
    return code;
  };

  s->Log("Install folder: " + PathText(install_dir));
  s->Log("Sources: " + PathText(source));
  std::error_code ec;
  CommitInfo head;
  std::string error;
  const bool git_checkout = fs::exists(source / ".git", ec);
  if (git_checkout) {
    s->Step("Getting " + branch + " (git)");
    const std::string src = PathText(source);
    if (run({"git", "-C", src, "fetch", "origin", branch}, source) != 0 ||
        run({"git", "-C", src, "checkout", "-f", "-B", branch, "origin/" + branch}, source) != 0) {
      return finish(Shared::Job::kFailed, "Getting the sources with git failed.");
    }
    std::string out, e;
    RunCapture({"git", "-C", src, "log", "-1", "--format=%H %cs %s"}, &out, &e);
    std::istringstream line(out);
    line >> head.sha >> head.date;
    std::getline(line, head.subject);
  } else {
    s->Step("Getting " + branch + " from GitHub");
    if (!BranchHead(repo, branch, &head, &error)) return finish(Shared::Job::kFailed, error);
    s->Log(Shorten(head.sha, 7) + " " + head.date + " " + head.subject);
    const fs::path tmp = source.parent_path() / ".amped3-download";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "x", ec);
    const fs::path archive = tmp / "sources.tar.gz";
    if (run({kCurl, "-fsSL", "--retry", "3", "-o", PathText(archive),
             "https://codeload.github.com/" + repo + "/tar.gz/" + head.sha},
            tmp) != 0) {
      return finish(Shared::Job::kFailed, "Downloading the sources failed.");
    }
    if (s->cancel) return finish(Shared::Job::kCancelled, "");
    if (run({kTar, "-xzf", PathText(archive), "-C", PathText(tmp / "x")}, tmp) != 0) {
      return finish(Shared::Job::kFailed, "Unpacking the sources failed.");
    }
    fs::path top;
    for (const auto& e : fs::directory_iterator(tmp / "x", ec)) {
      if (e.is_directory()) top = e.path();
    }
    if (top.empty() || !CopyTree(top, source, &error)) {
      return finish(Shared::Job::kFailed, error.empty() ? "The download was empty." : error);
    }
    WriteKeyValues(source / ".amped3-source",
                   {{"branch", branch}, {"commit", head.sha}, {"date", head.date}});
    fs::remove_all(tmp, ec);
  }
  if (s->cancel) return finish(Shared::Job::kCancelled, "");

  s->Step(install ? "Extracting the game and building (the first build takes a long time)"
                  : "Building");
  fs::create_directories(install_dir, ec);
  // The build runs from the sources' path without spaces (an install folder
  // picked with spaces; Windows short names: "Amped 3 Revamped" is AMPED3~1),
  // as some build tools don't take spaces.
  const fs::path build_src = NoSpacePath(source);
  if (build_src != source) s->Log("Building in " + PathText(build_src));
  std::vector<std::string> argv;
#if defined(_WIN32)
  argv = {"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
          PathText(build_src / "installer" / "Install-Amped3.ps1")};
  if (install) argv.insert(argv.end(), {"-Iso", PathText(iso)});
  else argv.insert(argv.end(), {"-GameDir", PathText(install_dir / "game")});
  argv.insert(argv.end(), {"-InstallDir", PathText(install_dir), "-Yes", "-UpscaleTextures",
                           install && upscale_on_install_ && kCanUpscale ? "yes" : "no"});
  if (install && FileContains(build_src / "installer" / "Install-Amped3.ps1", "$Graphics")) {
    argv.insert(argv.end(), {"-Graphics", original_look_ ? "original" : "revamped"});
  }
  // (an installer from before the launcher doesn't know it)
  if (FileContains(source / "installer" / "Install-Amped3.ps1", "NoShortcut")) argv.push_back("-NoShortcut");
#else
  argv = {"bash", PathText(build_src / "install.sh")};
  if (install) argv.insert(argv.end(), {"--iso", PathText(iso)});
  else argv.insert(argv.end(), {"--game-dir", PathText(install_dir / "game")});
  argv.insert(argv.end(), {"--install-dir", PathText(install_dir), "--yes", "--upscale-textures",
                           install && upscale_on_install_ && kCanUpscale ? "yes" : "no"});
  if (install && FileContains(build_src / "install.sh", "--graphics)")) {
    argv.insert(argv.end(), {"--graphics", original_look_ ? "original" : "revamped"});
  }
  // No questions, no Steam entry of its own (the launcher makes one); an
  // install.sh from before the launcher doesn't know it.
  if (FileContains(source / "install.sh", "--update)")) argv.push_back("--update");
#endif
  const int code = run(argv, build_src);
  if (code != 0) {
    return finish(Shared::Job::kFailed,
                  "The build failed (exit " + std::to_string(code) + "). Its logs are in " +
                      PathText(source / "logs") + ".");
  }
  if (!IsInstalled(install_dir)) {
    return finish(Shared::Job::kFailed, "The installer finished but the game isn't in " +
                                            PathText(install_dir) + ".");
  }
  WriteKeyValues(install_dir / "amped3-version.txt",
                 {{"branch", branch}, {"commit", head.sha}, {"date", head.date}});
  // The launcher moves into the install folder (the shortcuts point there),
  // leaving nothing in Downloads.
  RelocateLauncher(install_dir);
  finish(Shared::Job::kDone, "");
}

// Upscale textures: the install's texture upscaler (installer/Upscale-Textures.ps1,
// linux/upscale-textures.sh from its sources), then the game uses them.
void App::UpscaleMain() {
  auto s = s_;
  const fs::path install_dir = install_dir_;
  const fs::path source = source_dir_;
  auto finish = [&](Shared::Job result, const std::string& error) {
    std::lock_guard lock(s->m);
    s->job = s->cancel ? Shared::Job::kCancelled : result;
    s->job_error = s->cancel ? std::string("Stopped.") : error;
    s->job_end = std::chrono::steady_clock::now();
    if (s->log_file) {
      s->log_file << (result == Shared::Job::kDone ? "Finished.\n" : "Failed: " + error + "\n");
      s->log_file.close();
    }
  };
  std::error_code ec;
  s->Step("Upscaling textures (Real-ESRGAN): downloads 45 MB once, then a few minutes on the GPU");
  const fs::path src = NoSpacePath(source);
  std::vector<std::string> argv;
#if defined(_WIN32)
  const fs::path script = src / "installer" / "Upscale-Textures.ps1";
  argv = {"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PathText(script),
          "-InstallDir", PathText(install_dir)};
  for (const auto& entry : fs::directory_iterator(src / "out" / "build", ec)) {
    const fs::path tool = entry.path() / "amped3-texture-upscale.exe";
    if (fs::exists(tool, ec)) {
      argv.insert(argv.end(), {"-Tool", PathText(tool)});
      break;
    }
  }
#else
  const fs::path script = src / "linux" / "upscale-textures.sh";
  argv = {"bash", PathText(script), "--install-dir", PathText(install_dir)};
#endif
  if (!fs::exists(script, ec)) {
    return finish(Shared::Job::kFailed,
                  "The texture upscaler isn't in this install's sources (" + PathText(source) +
                      "): Update first.");
  }
  std::string error;
  const int code = RunProcess(argv, src, [&](const std::string& line) {
    if (line.rfind("==> ", 0) == 0) s->Step(line.substr(4));
    else s->Log(line);
  }, &error);
  if (code < 0) s->Log(error);
  if (code != 0) {
    return finish(Shared::Job::kFailed, "The texture upscaler failed (exit " + std::to_string(code) + ").");
  }
  // Used from now on (Graphics settings: Upscaled textures).
  GameConfig config;
  config.Load(install_dir / "amped3.toml");
  for (const auto& gs : DisplaySettings()) {
    if (std::string(gs.key) == "native_upscaled_textures") config.Set(gs, "true");
  }
  finish(Shared::Job::kDone, "");
}

// The game's dashboard icon beside the install (amped3-icon.ico / .png). The
// install scripts make it from the disc's default.xex with amped3-icon, built
// with the game; an install made before that has none, so it is made here,
// with the tool from the install or its build folders, when one is there.
void App::EnsureIcon() {
  std::error_code ec;
  if (fs::exists(install_dir_ / "amped3-icon.ico", ec) &&
      fs::exists(install_dir_ / "amped3-icon.png", ec)) {
    return;
  }
  const fs::path xex = install_dir_ / "game" / "default.xex";
  if (!fs::exists(xex, ec)) return;
#if defined(_WIN32)
  const char* tool_name = "amped3-icon.exe";
#else
  const char* tool_name = "amped3-icon";
#endif
  std::vector<fs::path> tools{install_dir_ / tool_name};
  for (const auto& entry : fs::directory_iterator(source_dir_ / "out" / "build", ec)) {
    if (entry.is_directory(ec)) tools.push_back(entry.path() / tool_name);
  }
  for (const auto& tool : tools) {
    if (!fs::exists(tool, ec)) continue;
    std::string out, err;
    if (RunCapture({PathText(tool), PathText(xex), PathText(install_dir_)}, &out, &err) == 0) return;
  }
}

void App::MakeShortcut() {
  EnsureIcon();
  RelocateLauncher(install_dir_);
  const fs::path self =
      fs::exists(InstalledLauncher(install_dir_)) ? InstalledLauncher(install_dir_) : ExePath();
#if defined(_WIN32)
  // The game's dashboard icon, made from default.xex at install.
  std::error_code icon_ec;
  const fs::path icon = install_dir_ / "amped3-icon.ico";
  const bool has_icon = fs::exists(icon, icon_ec);
  const std::string script =
      "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([IO.Path]::Combine("
      "[Environment]::GetFolderPath('Desktop'),'Amped 3 Revamped.lnk'));"
      "$s.TargetPath='" + PathText(self) + "';$s.WorkingDirectory='" + PathText(self.parent_path()) +
      "';" +
      (has_icon ? "$s.IconLocation='" + PathText(icon) + ",0';" : std::string()) +
      "$s.Save()";
  std::string out, err;
  const int code = RunCapture({"powershell.exe", "-NoProfile", "-Command", script}, &out, &err);
  notice_ = code == 0 ? "Desktop shortcut made." : "Couldn't make the shortcut.";
#else
  const fs::path apps = HomeDir() / ".local" / "share" / "applications";
  std::error_code ec;
  fs::create_directories(apps, ec);
  const fs::path desktop = apps / "amped3-revamped.desktop";
  {
    std::ofstream f(desktop, std::ios::trunc);
    f << "[Desktop Entry]\nType=Application\nName=Amped 3 Revamped\n"
         "Comment=Amped 3 (Xbox 360), native\nExec=\"" << PathText(self) << "\"\nPath="
      << PathText(self.parent_path()) << "\nTerminal=false\nCategories=Game;\n";
    // The game's dashboard icon, made from default.xex at install.
    if (fs::exists(install_dir_ / "amped3-icon.png", ec)) {
      f << "Icon=" << PathText(install_dir_ / "amped3-icon.png") << "\n";
    }
  }
  std::string out, err;
  if (RunCapture({"sh", "-c", "command -v steamos-add-to-steam"}, &out, &err) == 0 && !out.empty()) {
    const int code = RunCapture({"steamos-add-to-steam", PathText(desktop)}, &out, &err);
    notice_ = code == 0 ? "Added to Steam (restart Steam if it doesn't show)."
                        : "Couldn't add it to Steam; the menu entry is there.";
  } else {
    notice_ = "Added to the applications menu. In Steam: Games > Add a Non-Steam Game.";
  }
#endif
}

// File dialogs (SDL): their callbacks may run on another thread.
void App::BrowseDisc() {
  static const SDL_DialogFileFilter filters[] = {{"Disc images", "iso"}};
  auto* s = new std::shared_ptr<Shared>(s_);
  SDL_ShowOpenFileDialog(
      [](void* user, const char* const* files, int) {
        auto* sp = static_cast<std::shared_ptr<Shared>*>(user);
        {
          std::lock_guard lock((*sp)->m);
          if (!files) (*sp)->dialog_error = SDL_GetError();
          else if (files[0]) (*sp)->picked_disc = files[0];
        }
        delete sp;
      },
      s, window_, filters, 1, PathText(HomeDir()).c_str(), false);
}

void App::BrowseFolder() {
  auto* s = new std::shared_ptr<Shared>(s_);
  SDL_ShowOpenFolderDialog(
      [](void* user, const char* const* files, int) {
        auto* sp = static_cast<std::shared_ptr<Shared>*>(user);
        {
          std::lock_guard lock((*sp)->m);
          if (!files) (*sp)->dialog_error = SDL_GetError();
          else if (files[0]) (*sp)->picked_folder = files[0];
        }
        delete sp;
      },
      s, window_, PathText(install_dir_.parent_path()).c_str(), false);
}

std::vector<App::Row> App::Rows() const {
  std::lock_guard lock(s_->m);
  if (s_->job == Shared::Job::kRunning) return {kRowCancel};
  if (!IsInstalled(install_dir_)) {
    if (!kCanUpscale) return {kRowDisc, kRowBranch, kRowFolder, kRowLook, kRowInstall, kRowQuit};
    return {kRowDisc, kRowBranch, kRowFolder, kRowLook, kRowUpscaleOnInstall, kRowInstall, kRowQuit};
  }
  if (page_ != Page::kHome) {
    std::vector<Row> rows;
    for (size_t i = 0; i < PageSettings().size(); ++i) rows.push_back(Row(kRowSetting + int(i)));
    if (page_ == Page::kDisplay) rows.push_back(kRowEffects);
    rows.push_back(kRowBack);
    return rows;
  }
  std::vector<Row> rows = {kRowPlay, kRowGraphics};
  if (kCanUpscale) rows.push_back(kRowUpscale);
  for (Row r : {kRowBranch, kRowUpdate, kRowCheck, kRowDirect, kRowShortcut, kRowQuit}) rows.push_back(r);
  return rows;
}

const std::vector<GameSetting>& App::PageSettings() const {
  return page_ == Page::kEffects ? EffectSettings() : DisplaySettings();
}

// The graphics pages edit the install's amped3.toml (read again each time a
// page opens: the game rewrites it when its own menus change something).
void App::OpenPage(Page page) {
  page_ = page;
  cursor_ = 0;
  confirm_until_ = 0;
  if (page != Page::kHome) config_.Load(install_dir_ / "amped3.toml");
}

void App::Change(Row row, int step) {
  if (row == kRowBranch) {
    {
      std::lock_guard lock(s_->m);
      const int n = int(s_->branches.size());
      if (n < 2) return;
      s_->branch = (s_->branch + step + n) % n;
      s_->check = Shared::Check::kIdle;
    }
    SaveSettings();
    confirm_until_ = 0;
    if (IsInstalled(install_dir_)) check_at_ = ImGui::GetTime() + 0.6;
  } else if (row == kRowDisc && !discs_.empty()) {
    disc_ = (disc_ + step + int(discs_.size())) % int(discs_.size());
  } else if (row == kRowDirect || row == kRowLook || row == kRowUpscaleOnInstall) {
    Activate(row);
  } else if (row >= kRowSetting && size_t(row - kRowSetting) < PageSettings().size()) {
    if (!config_.Step(PageSettings()[size_t(row - kRowSetting)], step)) {
      notice_ = "Couldn't save " + PathText(install_dir_ / "amped3.toml") + ".";
    }
  }
}

void App::Activate(Row row) {
  const double now = ImGui::GetTime();
  switch (row) {
    case kRowPlay:
      Play();
      break;
    case kRowBranch:
      Change(row, 1);
      break;
    case kRowDisc:
      BrowseDisc();
      break;
    case kRowFolder:
      BrowseFolder();
      break;
    case kRowInstall:
      if (discs_.empty()) {
        notice_ = "Choose the disc image first.";
      } else if (now < confirm_until_) {
        confirm_until_ = 0;
        StartJob(true);
      } else {
        confirm_until_ = now + 6.0;
      }
      break;
    case kRowUpdate:
      if (now < confirm_until_) {
        confirm_until_ = 0;
        StartJob(false);
      } else {
        confirm_until_ = now + 6.0;
      }
      break;
    case kRowCheck:
      StartCheck();
      break;
    case kRowDirect:
      direct_start_ = !direct_start_;
      SaveSettings();
      break;
    case kRowShortcut:
      MakeShortcut();
      break;
    case kRowCancel:
      s_->cancel = true;
      CancelRunning();
      break;
    case kRowQuit:
      quit_ = true;
      break;
    case kRowGraphics:
      OpenPage(Page::kDisplay);
      break;
    case kRowLook:
      original_look_ = !original_look_;
      SaveSettings();
      break;
    case kRowUpscaleOnInstall:
      upscale_on_install_ = !upscale_on_install_;
      SaveSettings();
      break;
    case kRowUpscale:
      if (now < confirm_until_) {
        confirm_until_ = 0;
        StartJob(false, true);
      } else {
        confirm_until_ = now + 6.0;
      }
      break;
    case kRowEffects:
      OpenPage(Page::kEffects);
      break;
    case kRowBack:
      OpenPage(page_ == Page::kEffects ? Page::kDisplay : Page::kHome);
      break;
    default:
      if (row >= kRowSetting) Change(row, 1);
      break;
  }
}

void App::OnAction(Action action) {
  const auto rows = Rows();
  if (rows.empty()) return;
  cursor_ = std::clamp(cursor_, 0, int(rows.size()) - 1);
  switch (action) {
    case Action::kUp:
      cursor_ = (cursor_ + int(rows.size()) - 1) % int(rows.size());
      confirm_until_ = 0;
      break;
    case Action::kDown:
      cursor_ = (cursor_ + 1) % int(rows.size());
      confirm_until_ = 0;
      break;
    case Action::kLeft:
      Change(rows[size_t(cursor_)], -1);
      break;
    case Action::kRight:
      Change(rows[size_t(cursor_)], 1);
      break;
    case Action::kAccept:
      Activate(rows[size_t(cursor_)]);
      break;
    case Action::kBack:
      if (page_ != Page::kHome) Activate(kRowBack);
      break;
    default:
      break;
  }
}

void App::SetRenderer(SDL_Renderer* renderer) {
  title_.reset();
  if (renderer) title_ = std::make_unique<TitleLogo>(renderer);
}

void App::Draw(ImFont* font) {
  // What the dialogs and threads reported.
  {
    std::lock_guard lock(s_->m);
    if (!s_->picked_disc.empty()) {
      const fs::path p = PathFrom(s_->picked_disc);
      auto it = std::find(discs_.begin(), discs_.end(), p);
      if (it == discs_.end()) it = discs_.insert(discs_.end(), p);
      disc_ = int(it - discs_.begin());
      s_->picked_disc.clear();
    }
    if (!s_->picked_folder.empty()) {
      install_dir_ = PathFrom(s_->picked_folder);
      // A new install puts its build beside the game unless one was chosen.
      if (!IsInstalled(install_dir_)) source_dir_ = install_dir_ / "build" / "amped3revamped";
      s_->picked_folder.clear();
    }
    if (!s_->dialog_error.empty()) {
      notice_ = "The file dialog isn't available here (" + s_->dialog_error + ").";
      s_->dialog_error.clear();
    }
  }
  if (check_at_ > 0 && ImGui::GetTime() >= check_at_) {
    check_at_ = 0;
    StartCheck();
  }

  // A copy of the shared state for drawing.
  std::string branch, step, job_error, check_error, log_path;
  std::vector<std::string> log_tail, commits;
  Shared::Job job;
  Shared::Check check;
  CommitInfo remote;
  int behind, ahead;
  bool job_install, job_upscale;
  std::chrono::steady_clock::duration elapsed{};
  {
    std::lock_guard lock(s_->m);
    branch = s_->branches[size_t(s_->branch)];
    step = s_->step;
    job = s_->job;
    job_install = s_->job_install;
    job_upscale = s_->job_upscale;
    job_error = s_->job_error;
    check = s_->check;
    check_error = s_->check_error;
    remote = s_->remote;
    behind = s_->behind;
    ahead = s_->ahead;
    commits = s_->commits;
    log_path = PathText(s_->log_path);
    const size_t keep = 60;
    for (size_t i = s_->log.size() > keep ? s_->log.size() - keep : 0; i < s_->log.size(); ++i) {
      log_tail.push_back(s_->log[i]);
    }
    if (job == Shared::Job::kRunning) elapsed = std::chrono::steady_clock::now() - s_->job_start;
    else if (job != Shared::Job::kIdle) elapsed = s_->job_end - s_->job_start;
  }
  const bool installed = IsInstalled(install_dir_);
  const Version version = ReadVersion(install_dir_ / "amped3-version.txt");
  const bool same_branch = version.branch.empty() || version.branch == branch;
  const auto rows = Rows();
  cursor_ = std::clamp(cursor_, 0, int(rows.size()) - 1);

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;
  const float unit = std::max(display.y / 720.0f, 0.6f);
  const float text_px = 20.0f * unit, small_px = 15.0f * unit, title_px = 42.0f * unit;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackTop, kBackTop, kBackBottom, kBackBottom);

  const float pad = 22.0f * unit;
  const float width = std::min(display.x - 2 * pad, 860.0f * unit);
  const float x0 = std::round((display.x - width) * 0.5f);
  float y = std::round(display.y * 0.06f);
  // The title: the animated logo (plays once, then holds its last frame);
  // the name in text if it can't be shown.
  const float logo_h = std::round(130.0f * unit);
  SDL_Texture* logo = title_ ? title_->Frame(logo_h) : nullptr;
  if (logo) {
    float u0, v0, u1, v1;
    title_->Uv(&u0, &v0, &u1, &v1);
    const float logo_w = std::round(logo_h * title_->Aspect());
    dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(logo))), ImVec2(x0, y),
                 ImVec2(x0 + logo_w, y + logo_h), ImVec2(u0, v0), ImVec2(u1, v1));
    y += logo_h + small_px * 0.6f;
  } else {
    dl->AddText(font, title_px, ImVec2(x0, y), kGold, "AMPED 3 REVAMPED");
    y += title_px * 1.15f;
  }
  const std::string sub =
      !installed                  ? "Build the game from your own Amped 3 (Xbox 360) disc image"
      : page_ == Page::kDisplay   ? "Graphics settings: display"
      : page_ == Page::kEffects   ? "Graphics settings: effects"
                                  : "Installed version  " + VersionText();
  dl->AddText(font, small_px, ImVec2(x0, y), kDim, sub.c_str());
  y += small_px * 2.0f;

  struct RowText {
    Row row;
    std::string label, value;
    ImU32 color = kWhite;
    bool enabled = true;
  };
  std::vector<RowText> texts;
  const double now = ImGui::GetTime();
  for (Row r : rows) {
    RowText t{r};
    switch (r) {
      case kRowDisc:
        t.label = "Disc image";
        if (discs_.empty()) {
          t.value = "none found: select to browse";
          t.color = kGold;
        } else {
          t.value = PathText(discs_[size_t(disc_)].filename());
          if (discs_.size() > 1) t.value = "<  " + t.value + "  >";
        }
        break;
      case kRowBranch:
        t.label = installed ? "Update branch" : "Branch";
        t.value = "<  " + branch + "  >";
        break;
      case kRowFolder:
        t.label = "Install to";
        t.value = PathText(install_dir_);
        t.color = kDim;
        break;
      case kRowInstall: {
        t.label = "Build and install";
        const uint64_t free = FreeSpace(install_dir_);
        if (now < confirm_until_) {
          t.value = std::string("select again: ") + (original_look_ ? "Original" : "Revamped") +
                    (upscale_on_install_ && kCanUpscale ? ", upscaled textures" : "");
          t.color = kGold;
        } else {
          t.value = "needs about 40 GB (" + GiB(free) + " free)";
          t.color = free && free < kNeededBytes ? kRed : kDim;
        }
        break;
      }
      case kRowPlay:
        t.label = "Play";
        break;
      case kRowUpdate:
        t.label = "Update";
        if (now < confirm_until_) {
          t.value = "select again to start";
          t.color = kGold;
        } else if (check == Shared::Check::kRunning) {
          t.value = "checking...";
          t.color = kDim;
        } else if (check == Shared::Check::kFailed) {
          t.value = "couldn't check";
          t.color = kRed;
        } else if (check == Shared::Check::kDone) {
          if (!same_branch) {
            t.value = "switch to " + branch;
            t.color = kGold;
          } else if (behind == 0) {
            t.value = "up to date";
            t.color = kGreen;
          } else if (behind > 0) {
            t.value = std::to_string(behind) + " new change" + (behind == 1 ? "" : "s");
            t.color = kGold;
          } else {
            t.value = "rebuild from GitHub";
            t.color = kDim;
          }
        }
        break;
      case kRowCheck:
        t.label = "Check for updates";
        break;
      case kRowDirect:
        t.label = "Start the game directly";
        t.value = direct_start_ ? "On (hold LB / View or Shift for this)" : "Off";
        break;
      case kRowShortcut:
#if defined(_WIN32)
        t.label = "Desktop shortcut";
#else
        t.label = "Add to Steam / menu";
#endif
        break;
      case kRowCancel:
        t.label = "Stop";
        t.value = (job_upscale ? "upscaling " : job_install ? "installing " : "updating ") +
                  Elapsed(elapsed);
        t.color = kGold;
        break;
      case kRowQuit:
        t.label = "Quit";
        break;
      case kRowGraphics:
        t.label = "Graphics settings";
        break;
      case kRowLook:
        t.label = "Graphics";
        t.value = original_look_ ? "<  Original (as on the Xbox 360)  >"
                                 : "<  Revamped (new effects, 60 fps)  >";
        break;
      case kRowUpscaleOnInstall:
        t.label = "Upscale textures";
        t.value = upscale_on_install_ ? "<  Yes, after the build  >" : "<  No  >";
        break;
      case kRowUpscale:
        t.label = "Upscale textures";
        if (now < confirm_until_) {
          t.value = "select again to start";
          t.color = kGold;
        } else {
          t.value = "menus, HUD, loading screens (Real-ESRGAN, 4x)";
          t.color = kDim;
        }
        break;
      case kRowEffects:
        t.label = "Effects";
        t.value = ">";
        break;
      case kRowBack:
        t.label = "Back";
        break;
      default:
        if (r >= kRowSetting && size_t(r - kRowSetting) < PageSettings().size()) {
          const GameSetting& gs = PageSettings()[size_t(r - kRowSetting)];
          t.label = gs.label;
          t.value = config_.Text(gs);
          if (gs.kind != GameSetting::Kind::kToggle) t.value = "<  " + t.value + "  >";
          // The effects do nothing while Revamped Graphics is off.
          if (gs.effect && config_.Get(EffectSettings()[0]) != "true") t.color = kDim;
        }
        break;
    }
    texts.push_back(t);
  }

  const float row_h = text_px * 1.75f;
  const float value_x = x0 + width * 0.38f;
  const float rows_h = row_h * float(texts.size());
  dl->AddRectFilled(ImVec2(x0 - pad * 0.6f, y - pad * 0.5f),
                    ImVec2(x0 + width + pad * 0.6f, y + rows_h + pad * 0.5f), kPanel, 10 * unit);
  dl->AddRect(ImVec2(x0 - pad * 0.6f, y - pad * 0.5f),
              ImVec2(x0 + width + pad * 0.6f, y + rows_h + pad * 0.5f), kEdge, 10 * unit, 0,
              1.5f * unit);
  for (size_t i = 0; i < texts.size(); ++i) {
    const RowText& t = texts[i];
    const ImVec2 a(x0 - pad * 0.3f, y), b(x0 + width + pad * 0.3f, y + row_h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(int(i));
    ImGui::InvisibleButton("row", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) cursor_ = int(i);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      cursor_ = int(i);
      if (t.row == kRowBranch || (t.row == kRowDisc && discs_.size() > 1 && io.MousePos.x > value_x)) {
        Change(t.row, io.MousePos.x < value_x + width * 0.2f ? -1 : 1);
      } else {
        Activate(t.row);
      }
    }
    ImGui::PopID();
    const bool selected = cursor_ == int(i);
    if (selected) dl->AddRectFilled(a, b, kSelect, 6 * unit);
    const float ty = y + (row_h - text_px) * 0.5f;
    dl->AddText(font, text_px, ImVec2(x0, ty), selected ? kGold : kWhite, t.label.c_str());
    if (!t.value.empty()) {
      // Too long (a path): its end, after "...".
      std::string value = t.value;
      const float room = x0 + width - value_x;
      if (font->CalcTextSizeA(text_px, FLT_MAX, 0, value.c_str()).x > room) {
        size_t from = 0;
        while (from < value.size() &&
               font->CalcTextSizeA(text_px, FLT_MAX, 0, ("..." + value.substr(from)).c_str()).x > room) {
          ++from;
        }
        value = "..." + value.substr(from);
      }
      dl->AddText(font, text_px, ImVec2(value_x, ty), t.color, value.c_str());
    }
    y += row_h;
  }
  y += pad * 1.2f;

  // The information under the rows.
  const float line_h = small_px * 1.35f;
  const float bottom = display.y - small_px * 2.8f;
  auto line = [&](const std::string& text, ImU32 color) {
    if (y + line_h > bottom) return false;
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x0 + width, y + line_h), true);
    dl->AddText(font, small_px, ImVec2(x0, y), color, text.c_str());
    dl->PopClipRect();
    y += line_h;
    return true;
  };
  auto para = [&](const std::string& text, ImU32 color) {
    std::vector<std::string> lines;
    Wrap(font, small_px, text, width, &lines);
    for (const auto& l : lines) line(l, color);
  };
  if (!notice_.empty()) para(notice_, kGold);
  if (job != Shared::Job::kIdle) {
    if (job == Shared::Job::kRunning) {
      para(step, kGold);
    } else if (job == Shared::Job::kDone) {
      para(job_upscale    ? "Textures upscaled and switched on (Graphics settings: Upscaled "
                            "textures). Select Play."
           : job_install ? "Installed. Select Play."
                         : "Updated. Select Play.",
           kGreen);
    } else {
      para(job_error.empty() ? std::string("Failed.") : job_error, kRed);
      para("Full output: " + log_path, kDim);
    }
    y += line_h * 0.3f;
    const int fit = std::max(0, int((bottom - y) / line_h));
    const size_t from = log_tail.size() > size_t(fit) ? log_tail.size() - size_t(fit) : 0;
    for (size_t i = from; i < log_tail.size(); ++i) line(log_tail[i], kDim);
  } else if (installed && page_ != Page::kHome) {
    para("Saved to amped3.toml in the install folder at once; the game uses them the next "
         "time it starts. The same settings are in the game's own Graphics panel.",
         kDim);
    if (page_ == Page::kEffects) {
      para("The effects are part of Revamped Graphics: off with it.", kDim);
    }
  } else if (!installed) {
    para("Build and install downloads this project's sources from GitHub and builds Amped 3 "
         "Revamped from your disc image, into the folder above. Nothing from the game is "
         "downloaded. The first build takes about an hour on a PC and several hours on a handheld.",
         kDim);
#if defined(_WIN32)
    para("It needs Visual Studio 2022 Build Tools (C++), LLVM / Clang 20+ and Git; the installer "
         "offers to install what's missing (Windows asks for permission).",
         kDim);
#else
    para("On SteamOS and Armada it builds in a container (podman, included); elsewhere it uses "
         "Clang 20+, CMake and Ninja when installed.",
         kDim);
#endif
  } else if (check == Shared::Check::kFailed) {
    para(check_error.empty() ? std::string("Couldn't reach GitHub.") : check_error, kRed);
  } else if (check == Shared::Check::kDone && !remote.sha.empty()) {
    line("Latest on " + branch + ": " + Shorten(remote.sha, 7) + " (" + remote.date + ")  " +
             remote.subject,
         kWhite);
    if (!same_branch && ahead > 0) {
      line("This install has " + std::to_string(ahead) + " change" + (ahead == 1 ? "" : "s") +
               " that " + branch + " doesn't.",
           kDim);
    }
    if (!commits.empty()) {
      line(same_branch ? "New since this install:" : "Newest on " + branch + ":", kDim);
      for (const auto& c : commits) {
        if (!line("  " + c, kWhite)) break;
      }
    }
  }

  dl->AddText(font, small_px * 0.9f, ImVec2(x0, display.y - small_px * 1.9f), kFaint,
              page_ != Page::kHome ? "A / Enter: select     Left / Right: change     B / Esc: back"
                                   : "A / Enter: select     Left / Right: change     Mouse works too");
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace launcher
