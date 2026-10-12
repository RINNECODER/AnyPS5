// Rosetta direct path, game window (fork-only, macOS x86_64, run under arch -x86_64).
//
// A relinked title's main thread is handed to libSceVideoOut's host application
// (Aps5RunHostApplication, as libkernel's Aps5StartGuest does) and the "guest" runs on the thread it
// starts. Opening VideoOut from that thread must put a window owned by this process on screen
// (CGWindowList), titled, full-screen capable, in an app with a Quit menu item; closing the window
// or quitting must end the process with exit code 0. Each scenario runs in a child process, so its
// exit status is checked. ANYPS5_UNOBTRUSIVE_WINDOWS keeps its meaning: no Dock icon, parked window.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "prx/libSceVideoOut/include/DirectHostApplication.hpp"
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#include "SceTypes.hpp"
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" int APS5_VABI sceVideoOutOpen(int, int, int, const void*);
extern char** environ;

namespace {

std::string scenario;
std::string marker;

[[noreturn]] void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::fflush(stderr);
    _exit(1);
}

void Require(bool condition, const std::string& what) {
    if (!condition) Fail(what);
}

// The first application window of this process that CGWindowList reports, or 0.
CGWindowID OwnWindow(bool* onScreen) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    CGWindowID found = 0;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        if ([info[(__bridge NSString*)kCGWindowOwnerPID] intValue] != getpid()) continue;
        // Normal level, or floating for unobtrusive test runs; not the menu bar.
        if ([info[(__bridge NSString*)kCGWindowLayer] intValue] >= CGWindowLevelForKey(kCGMainMenuWindowLevelKey)) continue;
        CGRect bounds{};
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(__bridge NSString*)kCGWindowBounds], &bounds);
        if (bounds.size.width < 100 || bounds.size.height < 100) continue;
        found = [info[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
        *onScreen = [info[(__bridge NSString*)kCGWindowIsOnscreen] boolValue];
        break;
    }
    if (list != nullptr) CFRelease(list);
    return found;
}

bool HasMenuItem(NSMenu* menu, SEL action) {
    for (NSMenuItem* item in menu.itemArray)
        if (item.action == action || (item.submenu != nil && HasMenuItem(item.submenu, action))) return true;
    return false;
}

void Guest() {
    Require(pthread_main_np() == 0, "the guest runs off the main thread");
    const auto opened = std::chrono::steady_clock::now();
    Require(sceVideoOutOpen(255, 0, 0, nullptr) > 0, "open the video output");
    bool onScreen = false;
    CGWindowID window = 0;
    while ((window = OwnWindow(&onScreen)) == 0 || !onScreen) {
        Require(std::chrono::steady_clock::now() - opened < std::chrono::seconds(10),
            "a window owned by the process is on screen (found " + std::to_string(window) + ", on screen " + std::to_string(onScreen) + ")");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count();
    __block std::string problem;
    dispatch_sync(dispatch_get_main_queue(), ^{
        const bool unobtrusive = AnyPS5::Host::CurrentWindowEtiquette() != AnyPS5::Host::WindowEtiquette::Default;
        NSWindow* front = nil;
        for (NSWindow* candidate in NSApp.windows)
            if (candidate.windowNumber == static_cast<NSInteger>(window)) front = candidate;
        if (front == nil) problem = "the on-screen window is one of NSApp's windows";
        else if (![front.title isEqualToString:@"AnyPS5"]) problem = "the window is titled with the application title";
        else if ((front.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary) == 0) problem = "the window can go full screen";
        else if (NSApp.activationPolicy != (unobtrusive ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular))
            problem = "the activation policy is regular (accessory for unobtrusive test runs)";
        else if (!HasMenuItem(NSApp.mainMenu, @selector(terminate:)) || !HasMenuItem(NSApp.mainMenu, @selector(toggleFullScreen:)))
            problem = "the menu bar has Quit and Toggle Full Screen";
    });
    Require(problem.empty(), problem);
    std::ofstream(marker) << "window " << window << " on screen after " << seconds << " s\n";
    std::printf("window %u on screen %.2f s after sceVideoOutOpen; %s\n", window, seconds, scenario.c_str());
    std::fflush(stdout);
    const bool close = scenario == "close";
    dispatch_async(dispatch_get_main_queue(), ^{
        if (close) {
            for (NSWindow* candidate in NSApp.windows)
                if (candidate.windowNumber == static_cast<NSInteger>(window)) [candidate performClose:nil];
        } else {
            [NSApp terminate:nil];
        }
    });
    std::this_thread::sleep_for(std::chrono::seconds(10));
    Fail(close ? "closing the window ends the process" : "quitting ends the process");
}

void StartGuest(void*) { std::thread(Guest).detach(); }

// Runs one scenario in a child process; true when it showed the window and exited 0 by ending the game.
bool RunScenario(const char* self, const char* metallib, const char* name) {
    char directory[] = "/tmp/anyps5-direct-host-window-XXXXXX";
    if (mkdtemp(directory) == nullptr) return false;
    const std::string markerPath = std::string(directory) + "/window";
    const std::string logPath = std::string(directory) + "/stderr";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char* argv[] = {const_cast<char*>(self), const_cast<char*>(metallib), const_cast<char*>("--child"), const_cast<char*>(name),
        const_cast<char*>(markerPath.c_str()), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, self, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (spawned != 0 || waitpid(child, &status, 0) != child) return false;
    std::ifstream logFile(logPath);
    const std::string log((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    std::ifstream markerFile(markerPath);
    const bool shown = markerFile.good();
    const bool ended = log.find("ending the game") != std::string::npos;
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s: window shown %d, exit %d, ended the game %d\n", name, shown, WIFEXITED(status) ? WEXITSTATUS(status) : -1, ended);
    if (!(shown && clean && ended)) std::fprintf(stderr, "%s child stderr:\n%s\n", name, log.c_str());
    std::remove(markerPath.c_str());
    std::remove(logPath.c_str());
    rmdir(directory);
    return shown && clean && ended;
}

}

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[2], "--child") == 0) {
        setenv("ANYPS5_METAL_UTILITY_LIBRARY", argv[1], 1);
        unsetenv("ANYPS5_ROSETTA_HEADLESS");
        scenario = argv[3];
        marker = argv[4];
        Aps5RunHostApplication_nid_no_patch(StartGuest, nullptr);
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <AnyPS5Utilities.metallib>\n", argv[0]);
        return 2;
    }
    char self[PATH_MAX];
    uint32_t size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) != 0) return 2;
    bool passed = true;
    for (const char* name : {"close", "quit"}) passed = RunScenario(self, argv[1], name) && passed;
    if (passed) std::printf("rosetta direct host window: window on screen from a guest thread; close and quit exit 0\n");
    return passed ? 0 : 1;
}
