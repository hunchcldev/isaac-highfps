@echo off
rem Assembles the shippable download: release\isaac-highfps-<ver>.zip
rem The Workshop companion is maintained in the game's mods folder, not here.
setlocal
cd /d "%~dp0"
set VER=0.11.1

call "%~dp0build.bat" || exit /b 1

if exist release rmdir /s /q release
mkdir release\isaac-highfps

copy /y build\winmm.dll        release\isaac-highfps\ >nul
mkdir release\isaac-highfps\alternative-name >nul
copy /y build\opengl32.dll     release\isaac-highfps\alternative-name\ >nul
copy /y README.md              release\isaac-highfps\ >nul
copy /y isaac-highfps.ini.example release\isaac-highfps\isaac-highfps.ini >nul

powershell -NoProfile -Command ^
  "Compress-Archive -Path 'release\isaac-highfps\*' -DestinationPath 'release\isaac-highfps-%VER%.zip' -Force"
if errorlevel 1 (echo ZIP_FAILED & exit /b 1)

echo.
echo   release\isaac-highfps-%VER%.zip   ^<- upload this to GitHub Releases
exit /b 0
