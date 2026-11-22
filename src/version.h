// The single source of the release version. package.bat parses this line for the zip
// name, dllmain publishes it into the Lua state as HIGH_FPS_VERSION, and the Workshop
// companion compares it against the newest version it was uploaded with.
//
// Releasing therefore means: bump this, run package.bat, publish the zip - and bump
// LATEST in the companion's main.lua and re-upload it, or nobody gets told to update.
#pragma once
#define HIGHFPS_VERSION "0.14.0"
