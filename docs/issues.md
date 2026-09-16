3. System has become a catch-all. It holds CPU/RAM, six battery properties, the sandboxed file read, the Start menu, and the focus bracket (rememberFocus()/restoreFocus()). Every other hardware area got its own singleton with available, but battery is System.batteryAvailable. I'd move battery into Battery, or into Power, since Windows groups them. Renaming these breaks user plugins, so ship it as a feat!: rather than slipping it in.

Minor

- Duplicated Win32 helpers: processCreationTime is in both tilingapi.cpp:87 and tilingstate.cpp:39, and the exe-path lookup is in both foregroundwindow.cpp:54 and tilingapi.cpp:1004. The comments say one copy was deliberate. With two duplicated pairs, a small win32util.* starts to pay for itself.
- Services start whether or not anything uses them. The System timer, Wifi and Bluetooth polling, and the tiler's system-wide event hooks all run from main.cpp even when no loaded plugin reads them. Each costs little. If you care, connectNotify can start polling on the first binding.




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