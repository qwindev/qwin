# Third-party binaries

## VirtualDesktopAccessor.dll

- Source: https://github.com/Ciantic/VirtualDesktopAccessor (Jari Pennanen)
- Release: `2024-12-16-windows11` ("works with 24H2, 26100.2605")
- License: MIT (see `VirtualDesktopAccessor-LICENSE.txt`)

Wraps the undocumented Windows virtual-desktop COM interfaces (whose GUIDs
change between Windows builds) behind a stable C API. Loaded at runtime by
`virtualdesktops.cpp` from the exe directory; CMake copies it next to the
exe after every build. When updating Windows past a major build, check the
repo for a matching newer release.
