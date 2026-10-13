// amped3 - the launcher: a screen shown at start, before the game boots.
//
//   Play                      boots the game
//   Update branch  < main >   which branch Update gets (the repository's
//                             branches, listed from GitHub)
//   Update                    gets that branch and rebuilds this copy of the
//                             game (update.ps1 / update.sh in the project
//                             folder), showing their output; then Restart
//   Check for updates         what's new on the branch since this build
//   Show launcher at start    off: the game boots at once (hold LB / View or
//                             Shift while it starts to see the launcher)
//   Quit
// The version this build was made from is shown under the title.
//
// Controller: up / down choose, A selects, left / right change the branch.
// Keyboard: arrows, Enter. Mouse: click.
//
// The game's own project folder is found from source.path next to the exe
// (written by the installers and setup scripts). Settings: show_launcher and
// launcher_branch in amped3.toml.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include <rex/ui/imgui_dialog.h>

namespace amped3 {

// Whether the launcher should show at this start: the setting, or LB / View
// (controller) or Shift held. UI thread.
bool LauncherWanted();

// Deletes the *.old files an update left next to the exe (the files the
// running game held open). At start.
void CleanUpAfterUpdate(const std::filesystem::path& exe_dir);

struct LauncherState;  // launcher.cpp

class Launcher final : public rex::ui::ImGuiDialog {
 public:
  // `play` boots the game (called once, on the UI thread); `quit` closes the
  // app. The dialog deletes itself when closed.
  Launcher(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
           std::function<void()> play, std::function<void()> quit);
  ~Launcher() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void HandleInput();
  void Activate(int row);
  void ChangeBranch(int step);
  void StartCheck();
  void StartUpdate();
  void Restart();
  void SaveSettings();

  std::filesystem::path config_path_;
  std::function<void()> play_;
  std::function<void()> quit_;
  std::shared_ptr<LauncherState> state_;
  int cursor_ = 0;
  uint16_t prev_buttons_ = 0;
  bool first_frame_ = true;
  int held_direction_ = 0;
  double repeat_at_ = 0;
  double confirm_update_until_ = 0;  // "press again to update" while > now
  double check_at_ = 0;              // a check after the branch changed
};

}  // namespace amped3
