import AppKit
import SwiftUI

@main
struct AnyPS5LauncherApp: App {
    @NSApplicationDelegateAdaptor(LauncherDelegate.self) private var delegate
    @State private var store = LauncherStore()

    var body: some Scene {
        WindowGroup("AnyPS5", id: "launcher") {
            ContentView(store: store)
                .frame(minWidth: 1050, minHeight: 680)
                .onAppear { delegate.store = store }
                .task {
                    async let refresh: () = store.snapshot == nil ? store.refresh() : ()
                    async let probe: () = store.probeEngine()
                    _ = await (refresh, probe)
                }
        }
        .defaultSize(width: 1320, height: 850)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Add Local Executable…") { store.attach() }.keyboardShortcut("o")
                Button("Refresh Orbit Catalogue") { Task { await store.refresh() } }
                    .keyboardShortcut("r").disabled(store.isRefreshing)
            }
            CommandMenu("Engine") {
                Button("Choose AnyPS5 Runtime…") { store.chooseEngine() }
                    .disabled(store.isRunning || store.isProbingEngine)
                Button("Run Selected Game") { if let game = store.selectedLocal { store.launch(game) } }
                    .keyboardShortcut(.return).disabled(store.selectedLocal == nil || store.isRunning || store.isCleaningResources)
                Button("Stop Session") { store.stop() }.disabled(!store.isRunning)
                Toggle("Show Console", isOn: $store.showConsole).keyboardShortcut("l")
            }
        }
    }
}

@MainActor
final class LauncherDelegate: NSObject, NSApplicationDelegate {
    var store: LauncherStore?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let store, store.isRunning || store.hasMountedResources else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = store.isRunning ? "An AnyPS5 session is running" : "Game resources are still mounted"
        alert.informativeText = "Stop the session and detach its resource image before quitting?"
        alert.addButton(withTitle: "Keep Open")
        alert.addButton(withTitle: "Stop and Quit")
        if alert.runModal() == .alertSecondButtonReturn {
            Task {
                do {
                    try await store.stopAndWait()
                    sender.reply(toApplicationShouldTerminate: true)
                } catch {
                    store.error = "Resource cleanup failed; AnyPS5 stayed open. \(error.localizedDescription)"
                    sender.reply(toApplicationShouldTerminate: false)
                }
            }
            return .terminateLater
        }
        return .terminateCancel
    }
}
