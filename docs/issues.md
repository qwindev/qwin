New, most severe first

- A failed uncloak strands the window for good. showWindow at src/tilingapi.cpp:1670 ignores the return of the hider's show call and records the window as visible anyway. If explorer is mid-restart when you switch workspaces, the window stays cloaked, the next membership pass reads it as "the OS took it" and releases it, and from then on no state file, tray quit or recovery ever names it again. hideWindow already guards the symmetric case. Keep the hidden record on failure so the next switch or sweep retries.
- A hidden window whose monitor unplugs can land hidden on an active workspace. The hidden branch at src/tilingapi.cpp:1261 migrates it to the surviving monitor keeping its index, but never reconciles. If that index is the survivor's active workspace, the window is recorded hidden on an active workspace, which every placement, sweep and switch skips. Only Alt+Tab or switching away and back recovers it. Show it after the migrate when the index is active.
- A cross-monitor drag of the focused window leaves the focused monitor stale. The rescan migrate after a title-bar drag fires no foreground event, so m_focusedDevice still names the old monitor and the next switch chord acts on the wrong screen. This is the second half of the focused-monitor issue I reported; the fix is to update the device wherever the foreground window migrates.
- Float decisions no longer survive a minimize. The float flag now lives on the managed entry, which releaseWindow erases. A user-floated or sweep-floated window that is minimized and restored comes back tiled, and a sweep-floated one re-runs the three flickering rejections. The old code kept a per-HWND set across release. Also, toggling float on an overflow window is now a no-op, where before it made the float sticky so the window stopped being reclaimed.
- Recovered geometry is in the wrong coordinate space. A recovered entry's original is the placement's normal rect, which Windows reports in workspace coordinates, but every consumer hands it to SetWindowPos in screen coordinates. With the bar docked at the top, a post-recovery float toggle or disable places the window a bar height too high. placementShowCmd is saved and loaded but never read.

Lower priority, also verified

- The desktop-manager acquire in src/windowhider.cpp:137 has no negative cache. On a machine where it fails, every show event and every window per rescan re-creates the object and re-warns.
- effectiveHideMethod calls the acquiring variant per window per hide, even when the configured method is minimize.
- The reacquire test covers two RPC errors only. An explorer restart that surfaces as a different error leaves the dead proxy cached, so every window demotes to minimize and uncloaks fail silently.
- Removing a name from floatProcesses no longer re-tiles that app. The docs still describe it as live.
- A trusted state file with one zero HWND entry returns "nothing" instead of salvage, so the other entries are neither shown nor is the file deleted. Hard to reach in practice.
- Recovery hides real windows in the constructor and then guesses with a 5 s timer, although plugin loading is synchronous, so the pending set could be resolved deterministically after it.

Cleanup the agent flagged

processCreationTime is duplicated across the two units even though the tiler already includes the state header. fullExePathFor duplicates the foreground-window unit's image-path helper. The "did we hide this" predicate and the show-all teardown are each written more than once. retile arranges inactive-workspace trees whose placements are then discarded, and the workspaces Repeater rebuilds every delegate on each change.

The one item where the two reviews disagree on weight is the native desktop round trip. The agent rates it higher than I did because it also un-topmosts a pinned window while it sits on the other desktop and rewrites the state file without any of the released windows. Either way it is the behavior decision to make before committing.


when coming back from vdesktop, the focused window gets reaaranged