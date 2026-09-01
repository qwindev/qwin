# Building

Requirements: Qt 6.x (MSVC x64), Visual Studio 2022 Build Tools with the C++
workload, CMake 3.16+ and Ninja (both ship with the VC "C++ CMake tools"
component). Windows only.

From an **x64 Native Tools Command Prompt for VS 2022** (or any shell after
`vcvars64.bat`):

```cmd
:: Configure (adjust the Qt path to your install)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64

:: Build
cmake --build build

:: Run against the bundled examples
build\qwin.exe --plugins-dir plugins
```

If Ninja is unavailable, use the Visual Studio generator instead
(`-G "Visual Studio 17 2022" -A x64`, then `cmake --build build --config Release`).

When running `build\qwin.exe` directly, `C:\Qt\...\bin` must be on
`PATH`. To produce a self-contained folder that needs neither, see
[docs/packaging.md](packaging.md).

## Stopping the app

qwin quits from its tray menu; it keeps running with no visible windows when
no plugins are installed (`quitOnLastWindowClosed` is false, so bars and
popups closing must not kill the app). From a script, use:

```powershell
Stop-Process -Name qwin -Force
```
