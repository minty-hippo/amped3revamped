// amped3-launcher - the launcher's screens and jobs.
//
// First run (no game installed): pick the Amped 3 disc image, the branch and
// the folder; Build and install downloads the sources from GitHub and runs the
// project's installer (Install-Amped3.ps1 / install.sh), which extracts the
// game from the image and builds it. Nothing from the game is in the launcher
// or downloaded: the game comes from your own disc image.
//
// Afterwards: Play, Update (the chosen branch, rebuilt), Check for updates,
// Start the game directly (skip the launcher), a shortcut / Steam entry, and
// the version installed.

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"
#include "game_settings.h"
#include "title_logo.h"

struct ImFont;
struct SDL_Window;
struct SDL_Renderer;

namespace launcher {

enum class Action { kNone, kUp, kDown, kLeft, kRight, kAccept, kBack };

class App {
 public:
  App();
  ~App();

  // Whether to start the game at once (Start the game directly, unless held
  // keys / buttons ask for the launcher). Checked once at start.
  bool WantsDirectStart() const;
  // Starts the game; returns only on failure.
  void Play();

  void SetWindow(SDL_Window* window) { window_ = window; }
  // The renderer the title animation's texture lives in (null before it is
  // destroyed).
  void SetRenderer(SDL_Renderer* renderer);
  void OnAction(Action action);
  void Draw(ImFont* font);  // inside an ImGui frame
  bool QuitRequested() const { return quit_; }
  // The window should stay open (a job is running).
  bool Busy() const;

 private:
  struct Shared;  // state shared with the job and network threads

  enum Row {
    kRowDisc, kRowBranch, kRowFolder, kRowInstall,                       // setup
    kRowPlay, kRowUpdate, kRowCheck, kRowDirect, kRowShortcut,           // home
    kRowCancel, kRowQuit,
    kRowGraphics, kRowEffects, kRowBack,                                 // settings pages
    kRowSetting = 100  // + the setting's index on the page
  };
  enum class Page { kHome, kDisplay, kEffects };
  Page page_ = Page::kHome;
  GameConfig config_;
  const std::vector<GameSetting>& PageSettings() const;
  void OpenPage(Page page);

  std::vector<Row> Rows() const;
  void Activate(Row row);
  void Change(Row row, int step);
  void LoadSettings();
  void SaveSettings();
  void StartBranchList();
  void StartCheck();
  void StartJob(bool install);
  void JobMain(bool install, std::string branch, fs::path iso);
  void BrowseDisc();
  void BrowseFolder();
  void MakeShortcut();
  void EnsureIcon();
  std::string VersionText() const;

  std::shared_ptr<Shared> s_;
  SDL_Window* window_ = nullptr;
  std::unique_ptr<TitleLogo> title_;
  bool quit_ = false;
  int cursor_ = 0;
  double confirm_until_ = 0;
  double check_at_ = 0;
  std::string notice_;  // a line under the rows (shortcut made, dialog failed...)

  // Settings (launcher.ini).
  fs::path install_dir_;
  fs::path source_dir_;
  std::string repo_ = "minty-hippo/amped3revamped";
  bool direct_start_ = false;
  std::vector<fs::path> discs_;
  int disc_ = 0;
};

}  // namespace launcher
