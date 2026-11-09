@echo off
rem Makes subtitles for the game's dialogue on this PC from your own copy of
rem the game (offline speech recognition). See installer\Make-Subtitles.ps1 for
rem options (-InstallDir, -Model base.en/small.en, -Threads, -Force).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Make-Subtitles.ps1" %*
pause
