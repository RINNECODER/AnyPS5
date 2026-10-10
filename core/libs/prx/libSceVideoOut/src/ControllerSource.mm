#include "prx/libSceVideoOut/include/ControllerSource.hpp"
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace AnyPS5::Host {
namespace {
bool sourceOwned{};
std::uint64_t nextIdentity{1};
constexpr std::size_t maximumQueuedEvents = 4096;

void RequireMainThread() {
    if (![NSThread isMainThread]) throw std::logic_error("Controller source requires the main thread");
}

std::uint8_t StickByte(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Controller returned a non-finite stick value");
    const auto axis = static_cast<std::int32_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
    return static_cast<std::uint8_t>(((axis + 32768) * 255 + 32767) / 65535);
}

std::uint8_t TriggerByte(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Controller returned a non-finite trigger value");
    const auto axis = static_cast<std::int32_t>(std::clamp(value, 0.0f, 1.0f) * 32767.0f);
    return static_cast<std::uint8_t>((axis * 255 + 16383) / 32767);
}

GCControllerButtonInput* TouchpadButton(GCExtendedGamepad* profile) {
    if ([profile isKindOfClass:GCDualSenseGamepad.class]) return ((GCDualSenseGamepad*)profile).touchpadButton;
    if ([profile isKindOfClass:GCDualShockGamepad.class]) return ((GCDualShockGamepad*)profile).touchpadButton;
    return nil;
}

ControllerKind Kind(GCExtendedGamepad* profile) {
    if ([profile isKindOfClass:GCDualSenseGamepad.class]) return ControllerKind::DualSense;
    if ([profile isKindOfClass:GCDualShockGamepad.class]) return ControllerKind::DualShock;
    return ControllerKind::ExtendedGamepad;
}

ControllerState ReadState(GCExtendedGamepad* profile, ControllerKind kind, bool requiresTouchpad) {
    if (!profile) throw std::runtime_error("Controller capture lacks an extended gamepad profile");
    if (Kind(profile) != kind) throw std::runtime_error("Controller capture changed the physical profile kind");
    if (requiresTouchpad && !profile.buttons[GCInputDualShockTouchpadButton]) throw std::runtime_error("Controller capture lacks its physical touchpad button");
    ControllerState result;
    const auto add = [&](GCControllerButtonInput* button, ControllerButton target) {
        if (button.isPressed) result.buttons |= static_cast<std::uint32_t>(target);
    };
    add(profile.buttonA, ControllerButton::Cross);
    add(profile.buttonB, ControllerButton::Circle);
    add(profile.buttonX, ControllerButton::Square);
    add(profile.buttonY, ControllerButton::Triangle);
    add(profile.leftShoulder, ControllerButton::L1);
    add(profile.rightShoulder, ControllerButton::R1);
    add(profile.buttonMenu, ControllerButton::Options);
    add(profile.leftThumbstickButton, ControllerButton::L3);
    add(profile.rightThumbstickButton, ControllerButton::R3);
    add(profile.dpad.up, ControllerButton::Up);
    add(profile.dpad.right, ControllerButton::Right);
    add(profile.dpad.down, ControllerButton::Down);
    add(profile.dpad.left, ControllerButton::Left);
    add(profile.buttons[GCInputDualShockTouchpadButton], ControllerButton::TouchPad);
    if (kind == ControllerKind::ExtendedGamepad) add(profile.buttonOptions, ControllerButton::TouchPad);
    result.leftTrigger = TriggerByte(profile.leftTrigger.value);
    result.rightTrigger = TriggerByte(profile.rightTrigger.value);
    if (result.leftTrigger) result.buttons |= static_cast<std::uint32_t>(ControllerButton::L2);
    if (result.rightTrigger) result.buttons |= static_cast<std::uint32_t>(ControllerButton::R2);
    result.sticks = {StickByte(profile.leftThumbstick.xAxis.value), StickByte(-profile.leftThumbstick.yAxis.value),
                     StickByte(profile.rightThumbstick.xAxis.value), StickByte(-profile.rightThumbstick.yAxis.value)};
    return result;
}
}

struct NativeControllerSource::Impl : std::enable_shared_from_this<Impl> {
    mutable std::mutex mutex;
    ControllerSnapshot snapshot;
    std::deque<ControllerEvent> events;
    std::exception_ptr failure;
    GCController* selected = nil;
    GCExtendedGamepad* selectedProfile = nil;
    dispatch_queue_t previousQueue = nil;
    GCExtendedGamepadValueChangedHandler installedHandler = nil;
    id connectedObserver = nil;
    id disconnectedObserver = nil;
    id customizationObserver = nil;
    GCColor* defaultLight = nil;
    CHHapticEngine* haptics = nil;
    id<CHHapticPatternPlayer> rumble = nil;
    float rumbleLevel{};
    bool previousBackground{};
    bool ownsGlobal{};

    void CheckFailure() const {
        if (failure) std::rethrow_exception(failure);
    }

    void PublishLocked(ControllerEventKind kind) {
        try {
            if (snapshot.lastSequence == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Controller sequence exhausted");
            if (events.size() == maximumQueuedEvents) {
                events.pop_front();
                ++snapshot.droppedEvents;
            }
            ++snapshot.lastSequence;
            events.push_back({kind, std::chrono::steady_clock::now(), snapshot});
        } catch (...) {
            if (!failure) failure = std::current_exception();
            throw;
        }
    }

    template<class TAction> void Callback(TAction action) noexcept {
        try {
            @try {
                RequireMainThread();
                {
                    std::lock_guard lock(mutex);
                    if (!snapshot.sourceOpen || failure) return;
                }
                action();
            } @catch (NSException* error) {
                const char* reason = error.reason.UTF8String;
                throw std::runtime_error(reason ? reason : "GameController callback failed");
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure) failure = std::current_exception();
        }
    }

    void UpdateState(ControllerIdentity identity) {
        if (!selected) return;
        const ControllerState state = ReadState(selectedProfile.capture, Kind(selectedProfile), TouchpadButton(selectedProfile) != nil);
        std::lock_guard lock(mutex);
        if (!snapshot.sourceOpen || !snapshot.connected || snapshot.controller != identity || snapshot.state == state) return;
        snapshot.state = state;
        PublishLocked(ControllerEventKind::StateChanged);
    }

    void StopRumble() {
        @try {
            if (rumble) [rumble stopAtTime:CHHapticTimeImmediate error:nil];
            if (haptics) [haptics stopWithCompletionHandler:nil];
        } @catch (NSException*) {}
        rumble = nil;
        haptics = nil;
        rumbleLevel = 0;
    }

    void ApplyOutput(const ControllerOutput& output) {
        RequireMainThread();
        if (!selected) return;
        @try {
            if (GCDeviceLight* light = selected.light) {
                if (output.lightBar) {
                    const auto& color = *output.lightBar;
                    light.color = [[GCColor alloc] initWithRed:color[0] / 255.0f green:color[1] / 255.0f blue:color[2] / 255.0f];
                } else if (defaultLight) light.color = defaultLight;
            }
            const float level = std::max(output.largeMotor, output.smallMotor) / 255.0f;
            if (level == rumbleLevel) return;
            StopRumble();
            if (level == 0 || !selected.haptics) return;
            haptics = [selected.haptics createEngineWithLocality:GCHapticsLocalityDefault];
            if (!haptics || ![haptics startAndReturnError:nil]) { haptics = nil; return; }
            CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticIntensity value:level];
            CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                parameters:@[intensity] relativeTime:0 duration:GCHapticDurationInfinite];
            CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:nil];
            rumble = pattern ? [haptics createPlayerWithPattern:pattern error:nil] : nil;
            if (!rumble || ![rumble startAtTime:CHHapticTimeImmediate error:nil]) { StopRumble(); return; }
            rumbleLevel = level;
        } @catch (NSException*) {
            StopRumble();
        }
    }

    void Detach() {
        StopRumble();
        @try {
            if (selected.light && defaultLight) selected.light.color = defaultLight;
        } @catch (NSException*) {}
        defaultLight = nil;
        if (selectedProfile && selectedProfile.valueChangedHandler == installedHandler) {
            selectedProfile.valueChangedHandler = nil;
            selected.handlerQueue = previousQueue;
        }
        installedHandler = nil;
        selectedProfile = nil;
        selected = nil;
        previousQueue = nil;
    }

    void Attach(GCController* controller) {
        GCExtendedGamepad* profile = controller.extendedGamepad;
        if (!profile || controller.isSnapshot) throw std::invalid_argument("Controller is not a physical extended gamepad");
        if (profile.valueChangedHandler) throw std::runtime_error("Controller value handler is already owned");
        if (nextIdentity == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Controller identity exhausted");
        const ControllerIdentity identity{nextIdentity++, 1};
        std::string vendor;
        if (controller.vendorName) {
            const char* name = controller.vendorName.UTF8String;
            if (!name) throw std::runtime_error("Controller vendor name is not valid UTF-8");
            vendor = name;
        }
        selected = controller;
        selectedProfile = profile;
        defaultLight = controller.light.color;
        previousQueue = controller.handlerQueue;
        controller.handlerQueue = dispatch_get_main_queue();
        const std::weak_ptr<Impl> weak = shared_from_this();
        installedHandler = ^(GCExtendedGamepad*, GCControllerElement*) {
            if (auto self = weak.lock()) self->Callback([&] { self->UpdateState(identity); });
        };
        profile.valueChangedHandler = installedHandler;
        const ControllerState state = ReadState(profile.capture, Kind(profile), TouchpadButton(profile) != nil);
        std::lock_guard lock(mutex);
        snapshot.controller = identity;
        snapshot.kind = Kind(profile);
        snapshot.vendorName = std::move(vendor);
        snapshot.capabilities = {profile.leftThumbstickButton != nil, profile.rightThumbstickButton != nil,
                                 profile.buttonOptions != nil, TouchpadButton(profile) != nil, false, false, false};
        snapshot.state = state;
        snapshot.connected = true;
        PublishLocked(ControllerEventKind::Connected);
    }

    void Disconnect(GCController* controller) {
        if (!selected || selected != controller) return;
        Detach();
        std::lock_guard lock(mutex);
        snapshot.connected = false;
        snapshot.state = {};
        snapshot.kind = ControllerKind::Unknown;
        snapshot.capabilities = {};
        snapshot.vendorName.clear();
        PublishLocked(ControllerEventKind::Disconnected);
    }

    void Refresh() {
        NSArray<GCController*>* controllers = GCController.controllers;
        std::uint32_t unsupported{};
        GCController* candidate = nil;
        bool selectedPresent{};
        for (GCController* controller in controllers) {
            if (controller.isSnapshot) continue;
            if (!controller.extendedGamepad) {
                if (unsupported == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("Controller count exhausted");
                ++unsupported;
            } else if (!candidate) candidate = controller;
            if (controller == selected) selectedPresent = true;
        }
        if (selected && !selectedPresent) {
            Disconnect(selected);
        }
        {
            std::lock_guard lock(mutex);
            if (snapshot.unsupportedControllerCount != unsupported) {
                snapshot.unsupportedControllerCount = unsupported;
                PublishLocked(ControllerEventKind::AvailabilityChanged);
            }
        }
        if (!selected && candidate) Attach(candidate);
    }

    void Open() {
        RequireMainThread();
        if (sourceOwned) throw std::logic_error("A controller source is already open");
        sourceOwned = true;
        ownsGlobal = true;
        previousBackground = GCController.shouldMonitorBackgroundEvents;
        GCController.shouldMonitorBackgroundEvents = YES;
        snapshot.sourceOpen = true;
        const std::weak_ptr<Impl> weak = shared_from_this();
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        connectedObserver = [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            if (auto self = weak.lock()) self->Callback([&] { self->Refresh(); });
        }];
        disconnectedObserver = [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* event) {
            if (auto self = weak.lock()) self->Callback([&] {
                self->Disconnect(event.object);
                self->Refresh();
            });
        }];
        customizationObserver = [center addObserverForName:GCControllerUserCustomizationsDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* event) {
            if (auto self = weak.lock()) self->Callback([&] {
                if (event.object == self->selected) self->UpdateState(self->snapshot.controller);
            });
        }];
        Refresh();
    }

    void Close() {
        RequireMainThread();
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (connectedObserver) [center removeObserver:connectedObserver];
        if (disconnectedObserver) [center removeObserver:disconnectedObserver];
        if (customizationObserver) [center removeObserver:customizationObserver];
        connectedObserver = nil;
        disconnectedObserver = nil;
        customizationObserver = nil;
        Detach();
        if (ownsGlobal) {
            GCController.shouldMonitorBackgroundEvents = previousBackground;
            ownsGlobal = false;
            sourceOwned = false;
        }
        std::lock_guard lock(mutex);
        if (!snapshot.sourceOpen) return;
        snapshot.sourceOpen = false;
        snapshot.connected = false;
        snapshot.state = {};
        snapshot.kind = ControllerKind::Unknown;
        snapshot.capabilities = {};
        snapshot.vendorName.clear();
        snapshot.unsupportedControllerCount = 0;
        if (failure) return;
        try { PublishLocked(ControllerEventKind::SourceClosed); }
        catch (...) { failure = std::current_exception(); throw; }
    }
};

NativeControllerSource::NativeControllerSource(std::shared_ptr<Impl> value) : impl(std::move(value)) {}

std::unique_ptr<NativeControllerSource> NativeControllerSource::CreateMainThread() {
    RequireMainThread();
    auto value = std::make_shared<Impl>();
    try {
        @try { value->Open(); }
        @catch (NSException* error) {
            const char* reason = error.reason.UTF8String;
            throw std::runtime_error(reason ? reason : "GameController creation failed");
        }
        return std::unique_ptr<NativeControllerSource>(new NativeControllerSource(value));
    } catch (...) {
        const auto error = std::current_exception();
        try { value->Close(); } catch (...) {}
        std::rethrow_exception(error);
    }
}

NativeControllerSource::~NativeControllerSource() {
    bool open;
    { std::lock_guard lock(impl->mutex); open = impl->snapshot.sourceOpen; }
    if (!open) return;
    if (![NSThread isMainThread]) std::terminate();
    try { impl->Close(); } catch (...) { std::terminate(); }
}

void NativeControllerSource::CloseMainThread() { impl->Close(); }

void NativeControllerSource::ApplyOutputMainThread(const ControllerOutput& output) { impl->ApplyOutput(output); }

ControllerEventBatch NativeControllerSource::DrainEvents() {
    std::lock_guard lock(impl->mutex);
    impl->CheckFailure();
    ControllerEventBatch result;
    result.events.assign(impl->events.begin(), impl->events.end());
    result.after = impl->snapshot;
    impl->events.clear();
    return result;
}

ControllerSnapshot NativeControllerSource::Snapshot() const {
    std::lock_guard lock(impl->mutex);
    impl->CheckFailure();
    return impl->snapshot;
}
}
