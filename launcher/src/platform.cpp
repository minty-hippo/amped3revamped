// amped3-launcher - folders, settings and platform helpers (platform.h).

#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace launcher {

std::string PathText(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path PathFrom(const std::string& utf8) {
  return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

fs::path HomeDir() {
#if defined(_WIN32)
  if (const wchar_t* p = _wgetenv(L"USERPROFILE")) return fs::path(p);
  return fs::path(L"C:\\");
#else
  if (const char* p = std::getenv("HOME")) return fs::path(p);
  return fs::path("/");
#endif
}

fs::path ConfigDir() {
#if defined(_WIN32)
  if (const wchar_t* p = _wgetenv(L"APPDATA")) return fs::path(p) / L"Amped3Revamped";
  return HomeDir() / L"Amped3Revamped";
#else
  if (const char* p = std::getenv("XDG_CONFIG_HOME"); p && *p) {
    return fs::path(p) / "amped3-revamped";
  }
  return HomeDir() / ".config" / "amped3-revamped";
#endif
}

fs::path ExePath() {
#if defined(_WIN32)
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
  buf.resize(n);
  return fs::path(buf);
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
#endif
}

fs::path GameExe(const fs::path& install_dir) {
#if defined(_WIN32)
  return install_dir / L"amped3.exe";
#else
  return install_dir / "amped3";
#endif
}

bool IsInstalled(const fs::path& install_dir) {
  std::error_code ec;
  return !install_dir.empty() && fs::exists(GameExe(install_dir), ec) &&
         fs::exists(install_dir / "game" / "default.xex", ec);
}

fs::path DefaultInstallDir() {
  const fs::path games = HomeDir() / "Games";
  // An install made before the launcher (the installers' suggested folder).
  if (IsInstalled(games / "Amped 3 Revamped")) return games / "Amped 3 Revamped";
  // No spaces: the build tools run in a folder inside it.
  return games / "Amped3Revamped";
}

std::vector<fs::path> FindDiscImages() {
  std::vector<fs::path> dirs = {ExePath().parent_path(), HomeDir(), HomeDir() / "Downloads",
                                HomeDir() / "Documents", HomeDir() / "Desktop",
                                HomeDir() / "Games"};
#if !defined(_WIN32)
  // Removable media: /run/media/<user>/<label>, /media/<user>/<label>.
  for (const char* root : {"/run/media", "/media"}) {
    std::error_code ec;
    for (const auto& user : fs::directory_iterator(root, ec)) {
      std::error_code ec2;
      for (const auto& vol : fs::directory_iterator(user.path(), ec2)) dirs.push_back(vol.path());
    }
  }
#else
  // Other drives' roots.
  const DWORD drives = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {
    if (drives & (1u << i)) dirs.push_back(fs::path(std::wstring(1, wchar_t(L'A' + i)) + L":\\"));
  }
#endif
  std::vector<fs::path> found;
  auto consider = [&](const fs::path& p) {
    std::error_code ec;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".iso") return;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) < (1ull << 30)) return;
    const fs::path canon = fs::weakly_canonical(p, ec);
    if (std::find(found.begin(), found.end(), canon) == found.end()) found.push_back(canon);
  };
  for (const fs::path& dir : dirs) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      consider(e.path());
      // One level down (a folder per game).
      std::error_code ec2;
      if (e.is_directory(ec2) && !e.is_symlink(ec2)) {
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        std::error_code ec3;
        for (const auto& f : fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec3)) {
          consider(f.path());
        }
      }
    }
  }
  return found;
}

uint64_t FreeSpace(const fs::path& dir) {
  fs::path p = dir;
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) {
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  const auto info = fs::space(p, ec);
  return ec ? 0 : info.available;
}

std::map<std::string, std::string> ReadKeyValues(const fs::path& file) {
  std::map<std::string, std::string> values;
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
    values[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return values;
}

bool WriteKeyValues(const fs::path& file, const std::map<std::string, std::string>& values) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream f(file, std::ios::trunc);
  if (!f) return false;
  for (const auto& [k, v] : values) f << k << '=' << v << '\n';
  return bool(f);
}

bool ShiftHeld() {
#if defined(_WIN32)
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  return false;
#endif
}

bool InGamescope() {
#if defined(_WIN32)
  return false;
#else
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr ||
         (desktop && std::string(desktop) == "gamescope") ||
         std::getenv("SteamGamepadUI") != nullptr;
#endif
}

}  // namespace launcher
