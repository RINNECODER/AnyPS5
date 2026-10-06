import AppKit
import SwiftUI

@main
struct MacPSApp: App {
    @NSApplicationDelegateAdaptor(MacPSDelegate.self) private var delegate
    @State private var store: LauncherStore

    init() {
        _store = State(initialValue: LauncherStore())
    }

    var body: some Scene {
        Window("MacPS", id: "macps") {
            ConsoleRootView(store: store)
                .frame(minWidth: 1050, minHeight: 680)
                .onAppear { delegate.store = store }
                .task {
                    async let refresh: () = store.snapshot == nil ? store.refresh() : ()
                    async let probe: () = store.probeEngine()
                    _ = await (refresh, probe)
                }
        }
        .defaultSize(width: 1440, height: 850)
        .windowStyle(.hiddenTitleBar)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Add Game…") { store.attach() }.keyboardShortcut("o")
                Button("Refresh Catalogue") { Task { await store.refresh() } }
                    .keyboardShortcut("r").disabled(store.isRefreshing)
            }
            CommandMenu("Session") {
                Button("Run Selected Game") { if let game = store.selectedLocal { store.launch(game) } }
                    .keyboardShortcut(.return).disabled(store.selectedLocal == nil || store.isRunning || store.isCleaningResources || store.isInspectingGame || store.isProbingEngine)
                Button("Stop Session") { store.stop() }.disabled(!store.isRunning)
                Toggle("Show Technical Activity", isOn: $store.showConsole).keyboardShortcut("l")
            }
        }
    }
}

@MainActor
final class MacPSDelegate: NSObject, NSApplicationDelegate {
    var store: LauncherStore?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let store, store.isRunning || store.hasMountedResources else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = store.isRunning ? "A game session is running" : "Game resources are still mounted"
        alert.informativeText = "Stop the session and detach its resource image before quitting?"
        alert.addButton(withTitle: "Keep Open")
        alert.addButton(withTitle: "Stop and Quit")
        if alert.runModal() == .alertSecondButtonReturn {
            Task {
                do {
                    try await store.stopAndWait()
                    sender.reply(toApplicationShouldTerminate: true)
                } catch {
                    store.error = "Resource cleanup failed; MacPS stayed open. \(error.localizedDescription)"
                    sender.reply(toApplicationShouldTerminate: false)
                }
            }
            return .terminateLater
        }
        return .terminateCancel
    }
}
