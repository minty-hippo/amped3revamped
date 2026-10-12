@echo off
rem Upscales the game's textures 4x on this PC (Real-ESRGAN, any Vulkan GPU)
rem for the native renderer. See installer\Upscale-Textures.ps1 for options
rem (-InstallDir, -Set ui/world/all, -Model, -Tile, -Force).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Upscale-Textures.ps1" %*
pause
