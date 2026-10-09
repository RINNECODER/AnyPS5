#pragma once
// Opt-in window etiquette for test runs, selected by ANYPS5_UNOBTRUSIVE_WINDOWS.
//   unset/other  Default AppKit behaviour (real game runs): Dock icon, activation, key window.
//   "1"          No Dock icon and the process is never activated, so it cannot take keyboard focus.
//                Windows are parked in the bottom-right corner of the main screen at floating level:
//                still on screen and unoccluded (CAMetalLayer drawables and presentation behave as for
//                a front window), but small and out of the way.
//   "focus"      Same corner (normal level: an active process's windows are in front anyway), but the
//                process is still activated, for runs that verify real AppKit
//                key-window/focus behaviour. Such processes take a per-user lock first, so two
//                concurrent focus runs (parallel ctest, several CI runners) cannot steal focus from
//                each other mid-test.
// A test that verifies real focus calls RequireRealFocusForThisProcess() so that "1" is upgraded to
// "focus" for it; it never runs without activation.
#import <AppKit/AppKit.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace AnyPS5::Host {

enum class WindowEtiquette { Default, Unobtrusive, UnobtrusiveFocus };

inline WindowEtiquette CurrentWindowEtiquette() {
    const char* value = std::getenv("ANYPS5_UNOBTRUSIVE_WINDOWS");
    if (value == nullptr) return WindowEtiquette::Default;
    const std::string_view mode(value);
    if (mode == "1") return WindowEtiquette::Unobtrusive;
    if (mode == "focus") return WindowEtiquette::UnobtrusiveFocus;
    return WindowEtiquette::Default;
}

inline void RequireRealFocusForThisProcess() {
    if (CurrentWindowEtiquette() == WindowEtiquette::Unobtrusive) setenv("ANYPS5_UNOBTRUSIVE_WINDOWS", "focus", 1);
}

// Held until process exit; the kernel releases it even if the test crashes or is killed by a timeout.
// Keyboard focus belongs to one login session, so the lock lives in the user's private temporary directory
// (shared by local ctest and the self-hosted runners, which run as the same user). A focus run that cannot
// take the lock fails instead of activating unserialized.
inline void AcquireMachineFocusLock() {
    static const int descriptor = [] {
        char directory[PATH_MAX];
        const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
        if (length == 0 || length > sizeof(directory))
            throw std::runtime_error("Window focus lock: no per-user temporary directory");
        const std::string path = std::string(directory) + "/anyps5-window-focus.lock";
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "Window focus lock open " + path);
        int result;
        do result = flock(fd, LOCK_EX); while (result != 0 && errno == EINTR);
        if (result != 0) {
            const int error = errno;
            close(fd);
            throw std::system_error(error, std::generic_category(), "Window focus lock " + path);
        }
        return fd;
    }();
    (void)descriptor;
}

// Call before the window is first ordered on screen. Content size is left untouched.
inline void ParkUnobtrusively(NSWindow* window) {
    if (CurrentWindowEtiquette() == WindowEtiquette::Default || window == nil) return;
    NSScreen* screen = NSScreen.screens.firstObject;
    if (screen == nil) return;
    // Cascade leftwards so windows of one process do not cover each other.
    static CGFloat used = 0;
    const NSRect visible = screen.visibleFrame;
    const NSSize size = window.frame.size;
    if (used > 0 && used + size.width > visible.size.width) used = 0;
    [window setFrameOrigin:NSMakePoint(NSMaxX(visible) - used - size.width, NSMinY(visible))];
    used += size.width + 8;
    // An inactive process orders its windows behind the active app's; floating level keeps them visible.
    // A focus run is activated, so its windows are already in front at their normal level.
    if (CurrentWindowEtiquette() == WindowEtiquette::Unobtrusive) window.level = NSFloatingWindowLevel;
}

}
