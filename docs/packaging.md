# Packaging

Produce an xcopy-portable `dist\` folder that runs on machines without Qt
installed:

```cmd
mkdir dist
copy build\qwin.exe dist\
copy third_party\VirtualDesktopAccessor.dll dist\
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --qmldir plugins --qmldir deploy --release ^
  --no-compiler-runtime --no-translations --no-opengl-sw ^
  --no-system-d3d-compiler --no-system-dxc-compiler ^
  dist\qwin.exe
copy "%VCINSTALLDIR%Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\*.dll" dist\
xcopy /e /i plugins dist\plugins
```

The `plugins\` copy is what makes a fresh install work: on first launch
against the default location (`%APPDATA%\qwin\plugins\` missing entirely),
the exe seeds it from the `plugins\` folder beside it. An existing dir —
even an emptied one — is never touched, and `--plugins-dir` runs never seed.
CI does the same packaging in `.github/workflows/publish.yml`.

That produces a ~65 MB folder (~135 MB without the prune flags). Notes:

- Run windeployqt from a VS developer prompt so `VCINSTALLDIR` is set for the
  CRT copy on the last line.
- `--qmldir` must point at QML sources so windeployqt bundles the right QML
  modules. Users can import arbitrary QtQuick modules at runtime, so the
  second `--qmldir deploy` scans `deploy/kitchen-sink.qml`, which imports
  QtQuick, QtQuick.Window, QtQuick.Controls and QtQuick.Layouts — extend that
  file if your users need more modules. **Never narrow the QML deployment to
  what `plugins` alone imports**: the scanner only sees QML that
  exists at packaging time, and user plugins are written afterwards.
- The prune flags, and why each is safe here:
  - `--no-compiler-runtime` drops `vc_redist.x64.exe` (24 MB). Redundant once
    the `Microsoft.VC143.CRT` DLLs are copied app-local by the last line —
    ship one or the other, not both.
  - `--no-opengl-sw` drops `opengl32sw.dll` (20 MB), the software *OpenGL*
    rasterizer. Qt 6 renders Qt Quick through D3D11 (confirm with
    `QSG_INFO=1`), and D3D11's own software fallback is WARP, which ships
    with Windows. If a machine ever fails to render, `QT_QUICK_BACKEND=software`
    uses Qt Quick's raster backend, built into Qt6Quick.dll — no extra file.
  - `--no-system-dxc-compiler` drops `dxcompiler.dll` + `dxil.dll` (15 MB),
    needed only by the D3D12 RHI backend (`QSG_RHI_BACKEND=d3d12`).
  - `--no-system-d3d-compiler` drops `d3dcompiler_47.dll` (4.5 MB); it is the
    system copy, and Windows 8.1+ provides it from System32.
  - `--no-translations` drops 6.5 MB of Qt's own UI-string catalogs. Only
    affects strings Qt itself renders, not this app's.
- Verify the folder is really self-contained: clear PATH to
  `%SystemRoot%\system32;%SystemRoot%` before launching `dist\qwin.exe`,
  or you may be silently loading DLLs from the Qt install.
