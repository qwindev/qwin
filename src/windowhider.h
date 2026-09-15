#pragma once

// The hide backend behind the tiler's workspaces: two ways to make a window
// disappear without closing it.
//
//  - Cloak rides the compositor mechanism Windows uses to park a window on
//    another virtual desktop (DWMWA_CLOAKED). The call goes to explorer.exe's
//    own IApplicationView, never to the target or its process, so a hung
//    target cannot make SetCloak hang. Nothing here ever calls back into us,
//    so neither native-callback rule in CLAUDE.md applies.
//  - Minimize is the fallback: no ImmersiveShell, or a window that keeps
//    refusing to cloak. ShowWindowAsync, not ShowWindow, for the same
//    hang-immunity reason.
//
// Degrades like every other native unit: cloakAvailable() /
// onCurrentNativeDesktop() report a missing COM object and callers fall back
// (Method::Minimize; "assume yes"). State is process-global - one tiler, so
// one hider - and HWNDs travel as void*, as in windowfocus.h.
namespace hider {

enum class Method { None, Cloak, Minimize };

// Acquires the ImmersiveShell -> IApplicationViewCollection path if it has
// not been already: lazily, on the GUI thread, where Qt has initialized COM
// long before. False means Method::Cloak will fail - use Method::Minimize.
bool cloakAvailable();

// The same answer without acquiring anything, for callers that poll: through
// cloakAvailable() every tick would be a fresh CoCreateInstance, and on a
// machine where that keeps failing, a fresh warning with it.
bool cloakAcquired();

// False: the call failed (bad HWND, or explorer unreachable even after one
// re-acquire-and-retry) and the caller should fall back to Method::Minimize.
// Method::None is always true - there is nothing to do.
bool hide(void *hwnd, Method m);
bool show(void *hwnd, Method m);

// DWMWA_CLOAKED != 0: true for a window we cloaked, one the shell cloaked for
// another native desktop, or a dormant UWP host alike. Callers that need to
// tell them apart also check onCurrentNativeDesktop().
bool isCloaked(void *hwnd);

// IVirtualDesktopManager::IsWindowOnCurrentVirtualDesktop. True when the
// manager is unavailable or the query fails: a COM hiccup must never make a
// caller decide a window left its native desktop.
bool onCurrentNativeDesktop(void *hwnd);

} // namespace hider
