@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
rem Locate the 32-bit MSVC environment. vswhere ships with every VS 2017+ installer,
rem so this works for Build Tools, Community, Professional and Enterprise alike.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS (
    echo Could not find a Visual Studio installation with the C++ x86 toolset.
    echo Install "Desktop development with C++" ^(Build Tools are enough^) and retry.
    exit /b 1
)
call "%VCVARS%" >build\vcvars.log 2>&1
if errorlevel 1 (echo VCVARS_FAILED - see build\vcvars.log & exit /b 1)

if not exist src\winmm_thunks.cpp (
    echo generating winmm proxy thunks...
    python tools\gen_proxy.py >build\gen.log 2>&1
    if errorlevel 1 (echo GEN_FAILED & type build\gen.log & exit /b 1)
)
rem Same code under a second name, for the handful of setups where the loader never
rem picks up our winmm. opengl32 is imported by isaac-ng.exe too and is not a KnownDLL.
rem dbghelp was the first pick and got dropped: a crash handler can leave a real
rem dbghelp.dll in the game folder, and ours would shadow it. opengl32 in the folder is
rem normally just the loader's own from System32, so overwriting it is safe.
if not exist src\opengl32_thunks.cpp (
    echo generating opengl32 proxy thunks...
    python tools\gen_proxy.py "%SystemRoot%\SysWOW64\opengl32.dll" >>build\gen.log 2>&1
    if errorlevel 1 (echo GEN_FAILED & type build\gen.log & exit /b 1)
)

rem The mod ships as winmm.dll: isaac-ng.exe imports winmm statically, so the loader
rem maps us before main() runs. Uninstall = delete the file.
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /LD ^
   /Fo:build\ /Fe:build\winmm.dll ^
   src\dllmain.cpp src\winmm_thunks.cpp ^
   /link /DEF:src\winmm.def >build\dll.log 2>&1
if errorlevel 1 (echo CL_DLL_FAILED & type build\dll.log & exit /b 1)

if not exist build\alt mkdir build\alt
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /LD ^
   /Fo:build\alt\ /Fe:build\opengl32.dll ^
   src\dllmain.cpp src\opengl32_thunks.cpp ^
   /link /DEF:src\opengl32.def >build\dll_alt.log 2>&1
if errorlevel 1 (echo CL_ALT_FAILED & type build\dll_alt.log & exit /b 1)

rem Dev-only loader. NOT shipped - it exists so we can iterate without reinstalling.
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:build\ /Fe:build\inject.exe src\inject.cpp /link user32.lib >build\inject.log 2>&1
if errorlevel 1 (echo CL_INJECT_FAILED & type build\inject.log & exit /b 1)

echo BUILD_OK
exit /b 0
