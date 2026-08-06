@echo off
rem Assembles the two shippable halves:
rem   release\isaac-highfps-<ver>.zip   the native component users install by hand
rem   workshop\isaac-highfps\           the Workshop companion, ready for ModUploader
setlocal
cd /d "%~dp0"
set VER=0.11.0

call "%~dp0build.bat" || exit /b 1

if exist release rmdir /s /q release
mkdir release\isaac-highfps

copy /y build\winmm.dll        release\isaac-highfps\ >nul
mkdir release\isaac-highfps\alternative-name >nul
copy /y build\dbghelp.dll     release\isaac-highfps\alternative-name\ >nul
copy /y README.md              release\isaac-highfps\ >nul
copy /y isaac-highfps.ini.example release\isaac-highfps\isaac-highfps.ini >nul

powershell -NoProfile -Command ^
  "Compress-Archive -Path 'release\isaac-highfps\*' -DestinationPath 'release\isaac-highfps-%VER%.zip' -Force"
if errorlevel 1 (echo ZIP_FAILED & exit /b 1)

echo.
echo   release\isaac-highfps-%VER%.zip   ^<- upload this to GitHub Releases
echo   workshop\isaac-highfps\           ^<- point tools\ModUploader\ModUploader.exe at this
echo.
echo Workshop upload, first time:
echo   1. copy workshop\isaac-highfps into the game's mods\ folder
echo   2. run tools\ModUploader\ModUploader.exe, pick it, upload
echo   3. Steam assigns an id - paste it back into metadata.xml as ^<id^>
echo   4. metadata.xml ships with visibility Private; flip to Public when the
echo      GitHub release is actually live, or people subscribe to a dead link
exit /b 0
