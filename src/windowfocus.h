#pragma once

#include <functional>

// Handing the keyboard to a real application window. Shared by
// VirtualDesktops (a switch leaves the foreground on the desktop being left)
// and SystemApi (a popup that took focus closes again). HWNDs travel as
// void* to keep windows.h out of this header, like the rest of them.
namespace windowfocus {

// Visible, non-minimized, non-cloaked, unowned, Alt+Tab-able application
// window, neither ours nor the shell's own.
bool isFocusableAppWindow(void *hwnd);

// Activates the topmost focusable window (Z-order stands in for MRU),
// falling back to the shell, which is where Win+Ctrl+D leaves focus too.
// `accept` is an extra per-caller filter, e.g. "on the current desktop".
void focusTopmostWindow(const std::function<bool(void *)> &accept = {});

} // namespace windowfocus
