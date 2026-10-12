// Fork-only (Rosetta direct path, macOS x86_64): see DirectHostApplication.hpp.
#include "prx/libSceVideoOut/include/DirectHostApplication.hpp"
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#import <AppKit/AppKit.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

namespace {

std::string applicationTitle = "AnyPS5";

bool Headless() {
    const char* value = std::getenv("ANYPS5_ROSETTA_HEADLESS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// Read once on the main thread before the guest starts, so nothing races the metadata cache.
void LoadTitleAndIcon() {
    try {
        const auto title = GetAppTitle_nid_postfix();
        if (title.value[0] != '\0') applicationTitle.assign(title.value, strnlen(title.value, sizeof(title.value)));
    } catch (...) {
    }
    [NSProcessInfo.processInfo setProcessName:[NSString stringWithUTF8String:applicationTitle.c_str()] ?: @"AnyPS5"];
    try {
        if (!HasAppIcon_nid_postfix()) return;
        const auto icon = GetAppIconData_nid_postfix();
        NSImage* image = [[NSImage alloc] initWithData:[NSData dataWithBytes:icon.bytes length:icon.size]];
        if (image != nil) NSApp.applicationIconImage = image;
    } catch (...) {
    }
}

NSMenuItem* AddItem(NSMenu* menu, NSString* title, SEL action, NSString* key, NSEventModifierFlags modifiers = NSEventModifierFlagCommand) {
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    return item;
}

void InstallMainMenu() {
    NSString* name = [NSString stringWithUTF8String:applicationTitle.c_str()] ?: @"AnyPS5";
    NSMenu* bar = [NSMenu new];
    NSMenu* app = [[NSMenu alloc] initWithTitle:name];
    AddItem(app, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h");
    AddItem(app, @"Hide Others", @selector(hideOtherApplications:), @"h", NSEventModifierFlagOption | NSEventModifierFlagCommand);
    AddItem(app, @"Show All", @selector(unhideAllApplications:), @"");
    [app addItem:NSMenuItem.separatorItem];
    AddItem(app, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q");
    [bar addItemWithTitle:name action:nil keyEquivalent:@""].submenu = app;
    NSMenu* view = [[NSMenu alloc] initWithTitle:@"View"];
    AddItem(view, @"Toggle Full Screen", @selector(toggleFullScreen:), @"f", NSEventModifierFlagControl | NSEventModifierFlagCommand);
    [bar addItemWithTitle:@"View" action:nil keyEquivalent:@""].submenu = view;
    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    AddItem(window, @"Minimize", @selector(performMiniaturize:), @"m");
    AddItem(window, @"Zoom", @selector(performZoom:), @"");
    [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""].submenu = window;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
}

}

// Quit before the game window exists (the window's own delegate handles it afterwards and turns it
// into a close request, which DirectDisplayWindow ends the session on).
@interface AnyPS5DirectApplicationDelegate : NSObject<NSApplicationDelegate>
@end
@implementation AnyPS5DirectApplicationDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    AnyPS5::Host::EndDirectSession("quit");
    return NSTerminateCancel;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}
@end

namespace AnyPS5::Host {

namespace {
constexpr std::chrono::milliseconds kShutdownBudget{1000};
}

const std::string& DirectApplicationTitle() { return applicationTitle; }

void EndDirectSession(const char* reason) {
    static std::atomic<bool> ending{false};
    if (ending.exchange(true)) return;
    std::fprintf(stderr, "[videoout] %s: ending the game\n", reason);
    std::fflush(stderr);
    LibcBeginHostExit_nid_postfix();
    // Off the calling thread (it is usually the main thread, which shutdown callbacks may need), and
    // bounded: whatever the guest threads are doing, the process is gone after kShutdownBudget.
    std::thread([] {
        std::thread([] {
            std::this_thread::sleep_for(kShutdownBudget);
            std::fprintf(stderr, "[videoout] shutdown did not finish in %lld ms; leaving\n", static_cast<long long>(kShutdownBudget.count()));
            std::fflush(nullptr);
            std::_Exit(0);
        }).detach();
        try {
            LibcRunShutdown_nid_postfix();
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "[videoout] shutdown reported: %s\n", failure.what());
        } catch (...) {
            std::fprintf(stderr, "[videoout] shutdown reported an unknown error\n");
        }
        std::fflush(nullptr);
        std::_Exit(0);
    }).detach();
}

}

namespace {

// SIGTERM (MacPS, the launcher), SIGINT and SIGHUP end the game like closing the window. Dispatch sources
// on a global queue, so a busy or blocked main thread does not delay them.
void InstallStopSignals() {
    static dispatch_source_t sources[3];
    static const struct { int number; const char* name; } stops[] = {{SIGTERM, "SIGTERM"}, {SIGINT, "SIGINT"}, {SIGHUP, "SIGHUP"}};
    for (int index = 0; index < 3; ++index) {
        const char* name = stops[index].name;
        std::signal(stops[index].number, SIG_IGN);
        sources[index] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(stops[index].number), 0,
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        dispatch_source_set_event_handler(sources[index], ^{ AnyPS5::Host::EndDirectSession(name); });
        dispatch_resume(sources[index]);
    }
}

}

extern "C" [[noreturn]] void Aps5RunHostApplication_nid_no_patch(void (*startGuest)(void*), void* context) {
    if (![NSThread isMainThread]) {
        std::fprintf(stderr, "[videoout] the host application must run on the main thread\n");
        std::_Exit(134);
    }
    InstallStopSignals();
    if (Headless()) {
        startGuest(context);
        dispatch_main();
    }
    static AnyPS5DirectApplicationDelegate* delegate = nil;
    @autoreleasepool {
        [NSApplication sharedApplication];
        // Test runs (ANYPS5_UNOBTRUSIVE_WINDOWS) keep their no-Dock, no-focus etiquette.
        NSApp.activationPolicy = AnyPS5::Host::CurrentWindowEtiquette() == AnyPS5::Host::WindowEtiquette::Default
                                     ? NSApplicationActivationPolicyRegular
                                     : NSApplicationActivationPolicyAccessory;
        LoadTitleAndIcon();
        InstallMainMenu();
        delegate = [AnyPS5DirectApplicationDelegate new];
        NSApp.delegate = delegate;
        [NSApp finishLaunching];
    }
    startGuest(context);
    for (;;) [NSApp run];
}
