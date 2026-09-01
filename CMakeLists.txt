cmake_minimum_required(VERSION 3.16)

# Real versions come from git tags: CI configures with -DQWIN_VERSION=x.y.z
# (see .github/workflows/publish.yml). 0.0.0 marks a local dev build.
set(QWIN_VERSION "0.0.0" CACHE STRING "Build version, set from the git tag in CI")
project(qwin VERSION ${QWIN_VERSION} LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
qt_standard_project_setup()

qt_add_executable(qwin
    src/main.cpp
    src/pluginmanager.cpp src/pluginmanager.h
    src/pluginregistry.cpp src/pluginregistry.h
    src/systemapi.cpp src/systemapi.h
    src/colorpalette.cpp src/colorpalette.h
    src/panelwindow.cpp src/panelwindow.h
    src/virtualdesktops.cpp src/virtualdesktops.h
    src/hotkey.cpp src/hotkey.h
    src/wifiapi.cpp src/wifiapi.h
    src/windowfocus.cpp src/windowfocus.h
    src/mediaapi.cpp src/mediaapi.h
    src/audioapi.cpp src/audioapi.h
    src/foregroundwindow.cpp src/foregroundwindow.h
    src/layouttree.cpp src/layouttree.h
    src/tilingapi.cpp src/tilingapi.h
    src/powerapi.cpp src/powerapi.h
    src/bluetoothapi.cpp src/bluetoothapi.h
    src/appsapi.cpp src/appsapi.h
)

# Qt6::Widgets: QSystemTrayIcon
# shell32: SHAppBarMessage (PanelWindow), the AppsFolder namespace (Apps)
# dwmapi: DwmGetWindowAttribute, the cloaked-window test (windowfocus)
# wlanapi: Native WiFi (Wifi)
# windowsapp: WinRT umbrella lib for GSMTC (Media)
# ole32: MMDevice/AudioEndpointVolume COM (Audio)
# version: GetFileVersionInfo, for an exe's FileDescription (ActiveWindow)
# powrprof: GetPwrCapabilities/SetSuspendState (Power)
# bthprops + setupapi: device enumeration and the BLE battery property
#   (Bluetooth)
target_link_libraries(qwin PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets
    shell32 wlanapi iphlpapi dwmapi windowsapp ole32 version powrprof bthprops setupapi)

target_compile_definitions(qwin PRIVATE QWIN_VERSION_STR="${PROJECT_VERSION}")

# Hide the console window on release builds.
set_target_properties(qwin PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)

# virtualdesktops.cpp loads this from the exe directory at runtime.
add_custom_command(TARGET qwin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/VirtualDesktopAccessor.dll
        $<TARGET_FILE_DIR:qwin>)
