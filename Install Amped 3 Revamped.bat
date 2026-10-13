@echo off
rem Builds Amped 3 Revamped (a portable PC version) from your Amped 3 (Xbox 360) disc image.
rem See installer\Install-Amped3.ps1 for the options (-Iso, -InstallDir, ...).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Install-Amped3.ps1" %*
