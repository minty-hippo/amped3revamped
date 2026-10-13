// amped3-launcher - window, input and drawing (SDL3 + Dear ImGui).

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "app.h"
#include "platform.h"

namespace {

// A clean bold sans from the system (as the game's own menus); ImGui's
// built-in font otherwise.
ImFont* LoadFont(ImGuiIO& io) {
  std::vector<std::string> files;
#if defined(_WIN32)
  const char* windir = std::getenv("WINDIR");
  const std::string fonts = std::string(windir && *windir ? windir : "C:\\Windows") + "\\Fonts\\";
  for (const char* name : {"bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf"}) files.push_back(fonts + name);
#else
  for (const char* path : {"/usr/share/fonts/noto/NotoSans-Bold.ttf",
                           "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
                           "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                           "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                           "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
                           "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"}) {
    files.push_back(path);
  }
#endif
  for (const std::string& f : files) {
    if (SDL_GetPathInfo(f.c_str(), nullptr)) {
      if (ImFont* font = io.Fonts->AddFontFromFileTTF(f.c_str(), 20.0f)) return font;
    }
  }
  return io.Fonts->AddFontDefault();
}

}  // namespace

int main(int, char**) {
  SDL_SetHint(SDL_HINT_APP_NAME, "Amped 3 Revamped");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  launcher::App app;
  // Start the game directly (the setting), unless LB / View / Shift is held.
  SDL_PumpEvents();
  SDL_Delay(120);  // controllers connect
  SDL_PumpEvents();
  if (app.WantsDirectStart()) app.Play();  // returns only if it couldn't start

  const bool full = launcher::InGamescope();
  SDL_Window* window = SDL_CreateWindow("Amped 3 Revamped", 1280, 760,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                            (full ? SDL_WINDOW_FULLSCREEN : 0));
  if (!window) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  app.SetWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImFont* font = LoadFont(io);
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::vector<SDL_Gamepad*> pads;
  // Held directions repeat.
  launcher::Action held = launcher::Action::kNone;
  uint64_t repeat_at = 0;
  auto press = [&](launcher::Action a, bool repeats) {
    app.OnAction(a);
    held = repeats ? a : launcher::Action::kNone;
    repeat_at = SDL_GetTicks() + 380;
  };
  auto release = [&](launcher::Action a) {
    if (held == a) held = launcher::Action::kNone;
  };
  float axis_y = 0, axis_x = 0;

  bool running = true;
  while (running && !app.QuitRequested()) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      switch (e.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;
        case SDL_EVENT_GAMEPAD_ADDED:
          if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(g);
          break;
        case SDL_EVENT_KEY_DOWN:
          if (e.key.repeat) break;
          switch (e.key.key) {
            case SDLK_UP: press(launcher::Action::kUp, true); break;
            case SDLK_DOWN: press(launcher::Action::kDown, true); break;
            case SDLK_LEFT: press(launcher::Action::kLeft, false); break;
            case SDLK_RIGHT: press(launcher::Action::kRight, false); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: press(launcher::Action::kAccept, false); break;
            default: break;
          }
          break;
        case SDL_EVENT_KEY_UP:
          if (e.key.key == SDLK_UP) release(launcher::Action::kUp);
          if (e.key.key == SDLK_DOWN) release(launcher::Action::kDown);
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
          switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: press(launcher::Action::kUp, true); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: press(launcher::Action::kDown, true); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: press(launcher::Action::kLeft, false); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: press(launcher::Action::kRight, false); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: press(launcher::Action::kAccept, false); break;
            default: break;
          }
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
          if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) release(launcher::Action::kUp);
          if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) release(launcher::Action::kDown);
          break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
          const float v = float(e.gaxis.value) / 32767.0f;
          if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (v < -0.5f && axis_y >= -0.5f) press(launcher::Action::kUp, true);
            if (v > 0.5f && axis_y <= 0.5f) press(launcher::Action::kDown, true);
            if (std::abs(v) <= 0.5f && std::abs(axis_y) > 0.5f) {
              release(launcher::Action::kUp);
              release(launcher::Action::kDown);
            }
            axis_y = v;
          } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (v < -0.5f && axis_x >= -0.5f) press(launcher::Action::kLeft, false);
            if (v > 0.5f && axis_x <= 0.5f) press(launcher::Action::kRight, false);
            axis_x = v;
          }
          break;
        }
        default:
          break;
      }
    }
    if (held != launcher::Action::kNone && SDL_GetTicks() >= repeat_at) {
      app.OnAction(held);
      repeat_at = SDL_GetTicks() + 110;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app.Draw(font);
    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 6, 10, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    // Testing: AMPED3_LAUNCHER_ACTIONS=down,down,accept,... (one every 10
    // frames), then AMPED3_LAUNCHER_SCREENSHOT=<file.bmp> saves a frame (once
    // no job runs) and quits.
    static int frame = 0;
    ++frame;
    static std::vector<launcher::Action> script = [] {
      std::vector<launcher::Action> list;
      const char* env = std::getenv("AMPED3_LAUNCHER_ACTIONS");
      std::string rest = env ? env : "";
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string a = rest.substr(0, comma);
        rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        if (a == "up") list.push_back(launcher::Action::kUp);
        else if (a == "down") list.push_back(launcher::Action::kDown);
        else if (a == "left") list.push_back(launcher::Action::kLeft);
        else if (a == "right") list.push_back(launcher::Action::kRight);
        else if (a == "accept") list.push_back(launcher::Action::kAccept);
      }
      return list;
    }();
    static size_t script_at = 0;
    if (script_at < script.size() && frame % 10 == 0) app.OnAction(script[script_at++]);
    if (const char* shot = std::getenv("AMPED3_LAUNCHER_SCREENSHOT");
        shot && script_at == script.size() && frame >= 30 + int(script.size()) * 10 && !app.Busy()) {
      if (SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr)) {
        SDL_SaveBMP(surface, shot);
        SDL_DestroySurface(surface);
      }
      running = false;
    }
    SDL_RenderPresent(renderer);
    // Idle: no need to redraw faster.
    if (!app.Busy()) SDL_WaitEventTimeout(nullptr, 50);
  }

  for (SDL_Gamepad* g : pads) SDL_CloseGamepad(g);
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
