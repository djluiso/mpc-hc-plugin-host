@echo off
rem Builds MpcPluginHost.exe
rem   build.bat [x64^|x86]        (default x64)
rem Output: bin\<arch>\MpcPluginHost.exe and an empty bin\<arch>\Plugins folder.
rem Needs Visual Studio 2022 (or its Build Tools) with the C++ workload.

setlocal
set ARCH=%~1
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="x64" (set VCVARS=vcvars64.bat) else (set VCVARS=vcvars32.bat)

set ROOT=%~dp0
set OUT=%ROOT%bin\%ARCH%
set OBJ=%ROOT%bin\obj\%ARCH%

if defined VSINSTALLDIR goto :vsready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSDIR=
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (echo Visual Studio with C++ tools not found& exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1
:vsready

if not exist "%OUT%\Plugins" mkdir "%OUT%\Plugins"
if not exist "%OBJ%" mkdir "%OBJ%"

echo Building MpcPluginHost.exe
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /MP /Zi /DNOMINMAX /DUNICODE /D_UNICODE /Fo"%OBJ%\\" /Fd"%OBJ%\\" "%ROOT%src\*.cpp" ^
   /Fe"%OUT%\MpcPluginHost.exe" /link /SUBSYSTEM:WINDOWS /DEBUG /OPT:REF /OPT:ICF /PDB:"%OUT%\MpcPluginHost.pdb" || exit /b 1

echo Done: %OUT%
endlocal