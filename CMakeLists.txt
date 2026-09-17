cmake_minimum_required(VERSION 3.16)

# Real versions come from git tags: CI configures with -DQWIN_VERSION=x.y.z
# (see .github/workflows/publish.yml). 0.0.0 marks a local dev build.
set(QWIN_VERSION "0.0.0" CACHE STRING "Build version, set from the git tag in CI")
project(qwin VERSION ${QWIN_VERSION} LANGUAGES CXX)

# C++20, not Qt's default 17: under 17 C++/WinRT (mediaapi.cpp) falls back to
# <experimental/coroutine>, which MSVC 14.50+ rejects as removed.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
# REQUIRES 6.8 opts into QTP0001 NEW: qt_add_qml_module below resources its
# module under /qt/qml/, which is on the engine's default import path (the
# OLD prefix "/" is not, and warns). Qt is pinned at 6.8.3 here and in CI.
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(qwin
    src/main.cpp
    src/pluginmanager.cpp src/pluginmanager.h
    src/pluginregistry.cpp src/pluginregistry.h
    src/systemapi.cpp src/systemapi.h
    src/batteryapi.cpp src/batteryapi.h
    src/colorpalette.cpp src/colorpalette.h
    src/panelwindow.cpp src/panelwindow.h
    src/hotkey.cpp src/hotkey.h
    src/hotkeysapi.cpp src/hotkeysapi.h
    src/service.cpp src/service.h
    src/wifiapi.cpp src/wifiapi.h
    src/windowfocus.cpp src/windowfocus.h
    src/windowhider.cpp src/windowhider.h
    src/mediaapi.cpp src/mediaapi.h
    src/audioapi.cpp src/audioapi.h
    src/foregroundwindow.cpp src/foregroundwindow.h
    src/layouttree.cpp src/layouttree.h
    src/screendevice.h
    src/tilingapi.cpp src/tilingapi.h
    src/tilingapi_workspaces.cpp src/tilingapi_recovery.cpp src/tilingapi_p.h
    src/tilingstate.cpp src/tilingstate.h
    src/powerapi.cpp src/powerapi.h
    src/bluetoothapi.cpp src/bluetoothapi.h
    src/appsapi.cpp src/appsapi.h
)

# Qwin.Ui: host-provided QML compiled into the exe (Popup, PopupState,
# Theme). Plugins must all see the same PopupState instance for its
# "one open popup" rule to hold, which a copy vendored per plugin folder
# would break - so this is API, not sample content, and versions with the
# exe rather than living under plugins/.
# QT_QML_SINGLETON_TYPE marks PopupState/Theme as singletons in the
# generated qmldir; qt_add_qml_module alone doesn't infer it from `pragma
# Singleton`. QT_RESOURCE_ALIAS strips the src/ui/ prefix so the files land
# flat next to the module's qmldir instead of in a subdirectory of it.
set_source_files_properties(src/ui/PopupState.qml src/ui/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(_ui_file Popup.qml PopupState.qml Theme.qml)
    set_source_files_properties(src/ui/${_ui_file}
        PROPERTIES QT_RESOURCE_ALIAS ${_ui_file})
endforeach()
qt_add_qml_module(qwin
    URI Qwin.Ui
    VERSION 1.0
    QML_FILES src/ui/Popup.qml src/ui/PopupState.qml src/ui/Theme.qml
)

# Qt6::Widgets: QSystemTrayIcon
# shell32: SHAppBarMessage (PanelWindow), the AppsFolder namespace (Apps)
# dwmapi: DwmGetWindowAttribute, the cloaked-window test (windowfocus,
#   windowhider) and DWMWA_CLOAKED itself (windowhider)
# wlanapi: Native WiFi (Wifi)
# windowsapp: WinRT umbrella lib for GSMTC (Media)
# ole32: MMDevice/AudioEndpointVolume COM (Audio); ImmersiveShell and
#   IVirtualDesktopManager COM (windowhider)
# version: GetFileVersionInfo, for an exe's FileDescription (ActiveWindow)
# powrprof: GetPwrCapabilities/SetSuspendState (Power)
# bthprops + setupapi: device enumeration and the BLE battery property
#   (Bluetooth)
target_link_libraries(qwin PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets
    shell32 wlanapi iphlpapi dwmapi windowsapp ole32 version powrprof bthprops setupapi)

target_compile_definitions(qwin PRIVATE QWIN_VERSION_STR="${PROJECT_VERSION}")

# Hide the console window on release builds.
set_target_properties(qwin PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
