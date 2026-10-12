#pragma once
// Fork-only (Rosetta direct path, macOS x86_64): the AppKit application of a relinked title.
// libkernel's Aps5StartGuest hands the process main thread to Aps5RunHostApplication, which makes
// the process a regular foreground app (Dock icon, menu bar with Quit and full screen, the game's
// title and icon), starts the guest on its own thread and keeps the main thread in the AppKit run
// loop, so the main-queue work of DirectDisplayWindow (window, CAMetalLayer, input) runs.
#include <string>

extern "C" {
// Main thread only; never returns. `startGuest(context)` is called once the application is set up
// and must start the guest on another thread. ANYPS5_ROSETTA_HEADLESS=1 skips AppKit and only
// services the main dispatch queue.
[[noreturn]] void Aps5RunHostApplication_nid_no_patch(void (*startGuest)(void*), void* context);
}

namespace AnyPS5::Host {
// The game's title (param.json), or "AnyPS5" when the title has none.
const std::string& DirectApplicationTitle();
// Ends the game the way a user expects from closing it (window close, Quit, SIGTERM/SIGINT/SIGHUP):
// runs the libc shutdown callbacks (video output and GPU threads stop), flushes stdio and exits with
// code 0. Returns at once; the exit follows within one second whatever the guest threads are doing.
void EndDirectSession(const char* reason);
}
