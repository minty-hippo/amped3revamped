![alt text](https://github.com/minty-hippo/amped3revamped/blob/c53b30d0c8b10f9b75d2b94623ab7c9efb844b0a/Amped3Revamped%20logo.png "Amped 3 Revamped")
# Amped 3 Revamped

This is a recompilation of Amped 3 that has made use of ReXGlue and Claude to port the Xbox 360 code to modern platforms.

Features:
 - High resolution support, with display output settings, render resolution slider, XeSS and FSR options for upscaling the graphics.
 - Enhanced visuals, including camera and object-based motion blur (toggleable with intensity slider), speed blur, atmospheric fog, crepuscular sun rays, and a fog animation when entering and leaving the Trail Map.
 - EXPERIMENTAL high framerate support with some fixes for various minigames, this is still being worked on against a full playthrough of the game and all activities. Supports 30, 40, 60, 90, 120 and unlocked frame rates.
 - Modern Controller Support via xinput or SDL.
 - An in-game options menu that's controller navigable. F1 still works to access every option, as well as F4 for debug options.

What's been tested:
 - Northstar and Snowbird have been extensively tested, but I've yet to have tried the other parts of the game. Amped 3 Revamped includes a level and cutscene menu to replay completed story missions and cutscenes, as well as any that haven't been unlocked through normal gameplay.
 - It runs well on my Intel i7 358H laptop with 32gb of ram, around 60fps at a 50% resolution scale at 1080p or at 1440p with XESS.
 - The game runs via proton in SteamOS on a Ryzen 7700, 9070xt machine well, and less well but still functional on an Ayn Odin 3 running Armada.

WIP Features:
  - A texture upscaling system that runs on the host's device, allowing for higher resolution assets. I'm working on manually recreating the fonts and have remade the title logo at a higher resolution.
  - High framerate support is mostly working, but some side missions have had issues. The injury minigame has a scoring fix in place, as does turning of sleds and the hangglider at higher resolutions matching the turn speed of the standard game. 30 fps is consistent.
  - An x86/ARM Linux port, as I would like to test on a SteamOS desktop and on Armada on an AYN Odin 3. Proton works for both of these devices, but less translation would be preferable.
  - It seems like the launch Xbox 360 games might be single-threaded? Going to investigate further and see if multi-threading can be enabled to improve performance even further.

*Amped 3 (Xbox 360), running natively on Windows, with a new renderer and
enhancements.

This project statically recompiles Amped 3's `default.xex` into C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and builds a Windows
executable. On top of the plain recompile it adds:

- **Controller support.** SDL3 drives Xbox, PlayStation, Switch Pro and most
  other controllers, with hot-plugging. XInput is available as an alternative,
  and keyboard and mouse can stand in for a pad.
- **An in-game options menu** that you can drive entirely with a controller
  (**Back + Start**, or **F1**):
  - **Display:** fullscreen, output resolution (up to 4K), 16:9 letterbox,
    frame rate limit (30 console, 40, 60, 90, 120 or unlocked), render scale (1–3×),
    integer scaling, CAS/FSR upscaling and sharpening.
  - **Graphics:** anisotropic filtering (up to 16×), background shader
    compilation, GPU readback accuracy.
  - **Audio:** mute, buffer size.
  - **Controls:** controller driver, Guide button passthrough, keyboard/mouse mode
    and sensitivity.
  - **Enhancements:** game patch toggles (see *Adding an enhancement*) and
    experimental refresh-rate and frame-pacing controls.
  - **Cutscenes:** play, restart or skip any story cutscene while riding (for
    testing cutscene fixes).
  - **Challenges:** every challenge and story mission the game knows, by
    mountain; start any of the loaded mountain's (story missions already
    played included).

  Changes save to `amped3.toml` next to the exe. Settings that only apply after
  a relaunch are marked *(restart)*, and the menu lists any that are waiting.
  The menu only shows settings your SDK build actually has. While it is open,
  the game sees an idle controller.

## Folders

Nothing needs a particular name or place. The project folder can be called
anything, anywhere:

```
<project>\                 ← this project (unzip it anywhere)
├── Install Amped 3 Revamped.bat
├── <any folder>\          ← your extracted game, if you build without the installer
└── out\, logs\            ← created by the build
<project>\..\rexglue-sdk\  ← the SDK, created by setup.ps1 (or -SdkDir <folder>)
```

`setup.ps1` finds the extracted game by looking for `default.xex` in the
folders inside the project, then the folders next to it; pass `-GameDir
"<folder with default.xex>"` to name it directly.

The installed game is one portable folder; move it anywhere:

```
<install folder>\
├── amped3.exe, *.dll
├── amped3.toml       ← settings
├── game\             ← the game's files (from your disc)
├── save\             ← saves and profile
├── cache\            ← shader storage (safe to delete)
├── textures\         ← texture pack and upscaled textures
└── logs\
```

Saves used to go to `Documents\amped3`; the first start of this version copies
them into `save\` (the old folder is left as a backup). `--user_data_root`,
`--cache_root` and `--game_data_root` still point elsewhere if you want.

## Requirements

- Windows 10/11 x64.
- **Visual Studio 2022 or newer** (Community is fine), with the
  *Desktop development with C++* workload and these individual components:
  *C++ Clang Compiler for Windows* and *MSBuild support for LLVM (clang-cl)
  toolset*. CMake and Ninja come with it.
- **Clang 20 or newer.** If Visual Studio's bundled Clang is older,
  `winget install LLVM.LLVM` works too, and the script finds either one.
- **Git:** `winget install Git.Git`.
- Optional: turn on **Developer Mode** (Settings → System → For developers) so
  the SDK's symlinks check out properly. The script works around them if it's off.

## Install from the ISO (easiest)

Double-click **`Install Amped 3 Revamped.bat`** in the project folder. It will:

1. Ask for your Amped 3 (USA) disc image (`.iso`, a redump/XGD2 rip) and an
   install folder. The default is `Games\Amped 3 Revamped` in your user folder, and you need
   about 40 GB free.
2. Install anything missing with winget: VS 2022 Build Tools with the C++ and
   Clang components, LLVM, and Git. Pass `-NoToolInstall` to skip this.
3. Extract the game from the ISO into `<install folder>\game` and check its
   title ID. Nothing else is needed to read the ISO.
4. Run `setup.ps1` to build. The first time takes a long while.
5. Copy `amped3.exe`, its DLLs and `amped3.toml` into the install folder. This
   gives you a portable, self-contained folder you can move anywhere, and it
   can make a desktop shortcut.

Options (run from PowerShell, or append them to the .bat):

- `-Iso <path>`
- `-InstallDir <path>`
- `-GameDir <already-extracted folder>`
- `-Config RelWithDebInfo`
- `-ReExtract`
- `-Yes` (no prompts)
- `-Run`

Logs go to `logs\install-*.log`. Running it again skips the extraction and
rebuilds only what changed.

## Build

From a normal PowerShell window in the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1 -Run
```

The script checks your tools and clones the ReXGlue SDK at a pinned commit. It
applies the SDK fixes in `patches\rexglue`, builds and installs the SDK,
runs `rexglue init` against your game, and builds `out\build\win-amd64-release\amped3.exe`.

- The first run is long. The SDK takes a while to build. The recompiler then
  writes a few hundred MB of C++ to `generated\default\`, and compiling that
  is the slowest part.
- Rebuild later: `.\setup.ps1 -SkipSdk`
- Get past startup crashes: `.\setup.ps1 -SkipSdk -FixCrashes`. This launches the game, and whenever it crashes with *Call to invalid or unregistered function*, registers that address and rebuilds. It repeats until the game survives 45 seconds.
- Debuggable build for crashes: `.\setup.ps1 -SkipSdk -Config RelWithDebInfo`
- Game somewhere else: `.\setup.ps1 -GameDir "<folder with default.xex>"`

The exe uses the `game\` folder next to it. The build folder has none, so
`setup.ps1 -Run` (and `-FixCrashes`) start it with `--game_data_root` pointing
at the extracted game; the installer puts a `game\` folder in the install.

## Graphics

The game is drawn by the port's own Vulkan renderer (on by default; Options
(F1) > Graphics > Native renderer switches back to the GPU emulation).

**Revamped Graphics** turns all the visual enhancements on or off at once:
motion blur, speed blur, distance fog, god rays, branch fade and the trail
map clouds. Each keeps its own setting for when the switch is back on.

All of it is also in the game's own menu: **Pause > Options > Graphics** (the
row that was the unused "Cheat Codes") opens the port's Graphics panel. The
game's HUD and menus are hidden while it's open so you can see each change.
It has two tabs, switched with LB / RB:

- **Effects:** Revamped Graphics, a switch for each enhancement and the
  motion blur strength.
- **Display:** fullscreen, display resolution, render resolution (with the
  pixel size it gives), upscaler (Intel XeSS or AMD FSR 3.1) and FSR
  sharpness.

Up / down choose, left / right / A change, B goes back (keyboard: arrows,
Enter, Escape, Q / E for tabs).

**Sharp fonts.** The game's fonts are small bitmaps made for 720p, so at
higher resolutions its text is stretched and soft. The native renderer
rebuilds each of the five fonts (button icons included) at 4x from its own
letters the first time it's shown: each glyph's outline is traced into a
vector shape (straight edges, sharp corners, curves where the letter curves)
and filled anti-aliased at the new size. The result is kept in `logs\`
(`native_font_*.bin`) so later sessions load it at once; nothing is shipped. Switch it off
with Sharp Fonts (Graphics panel, Display tab) or `native_sharp_fonts`.

**Texture pack.** PNGs in the `textures` folder next to the exe replace game
textures by a hash of their contents, at any size (see
`assets/textures/README.txt`). Your own replacements go in
`assets/local/textures` (copied next to the exe by the build; kept out of the
published sources), for example a new logo for the start-up loading screen
and the title screen, made from `assets/local/amped3_logo.png` by
`tools/textures/make_logo_textures.py`. `native_texture_pack = false` turns
the pack off.

To draw the fonts by hand, `tools/textures/font_templates.py --game <game
folder> --out <folder>` writes each font's original image, a 4x guide (glyph
boxes, characters, baselines), a map (CSV) and the file name to save the
drawing under.

**Upscaled textures (optional).** The installer offers to upscale the game's
menu, HUD and loading screen textures 4x on your PC with
[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (ncnn/Vulkan build, any
GPU; downloaded once, 45 MB). `Upscale Textures.bat` runs the same step any
time (`-Set world` adds the characters, boards and levels, experimental;
`-Force` redoes everything). The results go to `textures\upscaled` and are
used only where neither the texture pack nor Sharp fonts replace a texture,
so anything you replace by hand always wins. Switch them off with Upscaled
Textures (Graphics panel, Display tab) or `native_upscaled_textures`.

The row is added in memory as the game loads its menus; the game's files are
never changed. Set `patch_game_menus = false` in `amped3.toml` to leave the
menus untouched.

## Hotkeys

| Key / buttons | Action |
|---|---|
| Back + Start (View + Menu), or F1 | Options menu |
| In menu: D-pad or left stick / A / B / LB-RB | Move / change / close / switch tabs |
| F9 | Hang report: every thread's stack, written to `logs\amped3_hang_*.txt` (use when the game is stuck) |
| F10 | Frame capture: the next frame's Direct3D calls and state, written to `logs\frame_*.txt` (see `docs/frame-capture.md`) |
| F3 | Performance overlay: frame rate, CPU and GPU frame times with a live graph, CPU and GPU usage, clock speed and power draw |
| F4 | Every SDK setting (advanced) |
| \` | Log console |

## When it breaks

Each step writes a log to `logs\` in this folder. Each game run writes
`logs\amped3_NNN.log` next to the exe. Send the newest one along with what
you saw. The usual problems and fixes:

| Symptom | Typical fix |
|---|---|
| Game crashes with *Call to invalid or unregistered function at guest address 0x…* | `setup.ps1 -SkipSdk -FixCrashes` registers these automatically. |
| Codegen warns about functions it couldn't size, or a crash points at an address inside a "function" | Add the function to `amped3_functions.toml` (`[functions]`). |
| Log shows `... STUB` or an unimplemented kernel export right before a hang | Implement or stub that export in a new `src/kernel_stubs.cpp`. |
| Missing HUD or menu text, black photos or reflections | Options → Graphics → GPU readback → Fast, then Full. |
| Hang on a loading screen after pressing Start | SDK patch 0001 (applied automatically). Check it applied in `logs\*-sdk-*.log`. |
| Striped green/orange movies | SDK patch 0002. Re-run codegen after applying it. |
| Release build renders something wrong but RelWithDebInfo is fine | Build Release at `-O2` (another port needed this). |

## Adding an enhancement

The game's machine code is already native C++ by the time the exe runs, so
there's nothing to patch in memory at runtime. An enhancement is a mid-asm
hook plus a setting that switches it on:

1. Find the instruction to change, for example the frame limiter for a 60 fps
   mode. Then register a hook in `amped3_functions.toml`:
   ```toml
   [[midasm_hook]]
   name = "Amped3_UnlockFramerate"
   address = 0x82123460        # the instruction you found
   registers = ["r3"]
   ```
2. Implement the hook in a new source file (add it to `overlay/CMakeLists.txt`
   as well). Gate it on a setting in the **`Amped 3/Patches`** category:
   ```cpp
   #include <rex/cvar.h>
   #include <rex/ppc/context.h>  // PPCRegister
   REXCVAR_DEFINE_BOOL(unlock_framerate, false, "Amped 3/Patches",
                       "Runs gameplay at 60 fps instead of 30.");
   void Amped3_UnlockFramerate(PPCRegister& r3) {
     if (REXCVAR_GET(unlock_framerate)) r3.u64 = 1;
   }
   ```
3. Re-run the build. The toggle appears under Options → Enhancements →
   Game patches without any menu changes.

## What's in here

| Path | Purpose |
|---|---|
| `setup.ps1` | Builds the SDK and the port, with logs |
| `src/amped3_app.*` | App hooks: GPU plugin, game path, menu hotkeys, input gating |
| `src/options_menu.*` | The options menu |
| `src/pad_bridge.*` | Controller reads for the menu, and keeping the game's input idle while it's open |
| `overlay/CMakeLists.txt` | Build file, copied in after `rexglue init` |
| `amped3_functions.toml` | Your recompiler hints (loaded last, so they win) |
| `docs/d3d-map.md` | Map of the game's Direct3D layer: functions, device layout, render-state table, hook plan |
| `amped3_d3d_names.toml` | Readable names for those functions (always applied; the capture hooks use them) |
| `src/frame_capture.*`, `docs/frame-capture.md` | F10 frame capture and shader dumps |
| `docs/native-renderer-plan.md` | Step-by-step plan for the native renderer |
| `tools/d3d-map/` | Scripts that produced the map |
| `amped3_functions_scan.toml` | 3,150 pointer-only functions found by a static scan of `default.xex` (code outside every `.pdata` function, referenced from vtables/callback tables or built with `lis`/`addi`) |
| `amped3_functions_auto.toml` | Function entry points `setup.ps1` adds when codegen reports branches into unknown code |
| `amped3.defaults.toml` | Starting `amped3.toml` (windowed, 16× AF) |
| `patches/rexglue/` | SDK fixes, credit [3on3NHL-recomp](https://github.com/Wndwsqrrl/3on3NHL-recomp) (MIT) |

Never share `generated\`, `out\`, the game folder or a built `amped3.exe`.
They contain or are built from the game's code. `.gitignore` keeps them, the
XeSS headers and `assets/local` out of git.

## License and credits

This project's own code is released into the public domain (the
[Unlicense](https://unlicense.org/), see `LICENSE`): no copyright is claimed
and no license terms apply to it. Third-party code keeps its own license, as
listed below. It contains no game code or data: you need your own copy of
Amped 3 for the Xbox 360. Amped 3 and its logo are trademarks of their
respective owners; this project is not affiliated with or endorsed by them.

### Credits

- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** © 2026 Tom Clay,
  portions derived from **[Xenia](https://xenia.jp)** © 2022 Ben Vanik and Xenia
  contributors (BSD 3-Clause). The SDK also brings SDL3, Dear ImGui, fmt,
  spdlog, glslang, toml++, xxHash and others under their own licenses.
- **SDK patches** adapted from
  [3on3NHL-recomp](https://github.com/Wndwsqrrl/3on3NHL-recomp) © 2026 Sage Grohs
  (MIT; `patches/rexglue/LICENSE-3on3NHL-recomp.txt`).
- **[miniz](https://github.com/richgel999/miniz)**: Rich Geldreich, RAD Game
  Tools and Valve Software (MIT; `third_party/miniz/LICENSE`).
- **[stb_image / stb_image_write](https://github.com/nothings/stb)**: Sean
  Barrett (public domain / MIT).
- **[Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers)**: The
  Khronos Group (Apache 2.0 / MIT; `third_party/vulkan/LICENSE.md`).
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)**
  (FSR 3.1 API headers) © Advanced Micro Devices, Inc. (MIT;
  `third_party/ffx_api/LICENSE.txt`). The runtime DLL is downloaded at build time.
- **[Intel XeSS](https://github.com/intel/xess)** © Intel Corporation (Intel
  Simplified Software License). Headers and runtime are downloaded at build
  time, not included here.
- **[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)** (ncnn-vulkan build)
  © Xintao Wang (BSD 3-Clause), for the optional texture upscaling. Downloaded
  at install time, not included here.*
