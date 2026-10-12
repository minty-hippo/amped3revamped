![alt text](https://github.com/minty-hippo/amped3revamped/blob/0785842a9e1518827f71296c0910ea0e25d44390/Amped3Revamped%20logo.png "Amped 3 Revamped")
# amped3revamped
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
