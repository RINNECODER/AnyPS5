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
                    .keyboardShortcut(.return).disabled(store.selectedLocal == nil || store.isRunning)
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
        guard let store, store.isRunning else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "An AnyPS5 session is running"
        alert.informativeText = "Stop the session before quitting?"
        alert.addButton(withTitle: "Keep Running")
        alert.addButton(withTitle: "Stop and Quit")
        if alert.runModal() == .alertSecondButtonReturn { store.stop(); return .terminateNow }
        return .terminateCancel
    }
}
