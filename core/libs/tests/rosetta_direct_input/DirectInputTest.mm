// Rosetta direct path, keyboard to pad (fork-only, macOS x86_64, run under arch -x86_64).
//
// A key pressed in the direct path's native AppKit window reaches the title's pad: the video output
// opens the window, a Space key-down is posted to it, and scePadReadState reports Cross (the
// default binding) until the key-up. This is the path a player without a controller uses.
// The window is activated (ANYPS5_UNOBTRUSIVE_WINDOWS=focus parks it in a corner and serializes
// focus with other focus tests), because a window only receives keys while its process is active.
#import <AppKit/AppKit.h>
#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" {
int APS5_VABI sceVideoOutOpen(int, int, int, const void*);
int APS5_VABI scePadInit_nid_postfix(void);
int APS5_VABI scePadOpen_nid_postfix(int, int, int, const void*);
int APS5_VABI scePadReadState(int, PadData*);
}

namespace {

constexpr std::uint32_t Cross = 0x4000;
constexpr unsigned short SpaceKeyCode = 49;

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

template<class Predicate> bool Within(std::chrono::milliseconds limit, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

bool WindowFocused() {
    __block bool focused = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSWindow* window = NSApp.windows.firstObject;
        focused = window != nil && NSApp.active && window.keyWindow;
    });
    return focused;
}

void PostSpace(bool down) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSWindow* window = NSApp.windows.firstObject;
        NSEvent* event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
            modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil
            characters:@" " charactersIgnoringModifiers:@" " isARepeat:NO keyCode:SpaceKeyCode];
        [NSApp postEvent:event atStart:NO];
    });
}

std::uint32_t Buttons(int pad) {
    PadData data{};
    Require(scePadReadState(pad, &data) == 0, "read the pad");
    return data.buttons;
}

void Guest() {
    Require(scePadInit_nid_postfix() == 0, "initialize the pad library");
    const int pad = scePadOpen_nid_postfix(1, 0, 0, nullptr);
    Require(pad > 0, "open the pad");
    Require(sceVideoOutOpen(255, 0, 0, nullptr) > 0, "open the video output");
    Require(Within(std::chrono::seconds(15), WindowFocused), "the native window opens and becomes the active key window");
    Require((Buttons(pad) & Cross) == 0, "Cross is up before any key");

    PostSpace(true);
    Require(Within(std::chrono::seconds(5), [&] { return (Buttons(pad) & Cross) != 0; }), "Space in the window holds Cross on the pad");
    PostSpace(false);
    Require(Within(std::chrono::seconds(5), [&] { return (Buttons(pad) & Cross) == 0; }), "releasing Space releases Cross");
}

}

int main() {
    setenv("ANYPS5_UNOBTRUSIVE_WINDOWS", "focus", 1);
    std::atomic<int> result{-1};
    std::thread guest([&] {
        try {
            Guest();
            result = 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            result = 1;
        }
    });
    while (result.load() < 0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    guest.join();
    if (result == 0) std::printf("rosetta direct input: Space in the window drives Cross on the pad\n");
    std::fflush(stdout);
    std::fflush(stderr);
    // The video output thread keeps running; skip its teardown.
    _exit(result.load());
}
