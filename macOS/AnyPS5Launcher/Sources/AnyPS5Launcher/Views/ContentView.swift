import SwiftUI

struct ContentView: View {
    @Bindable var store: LauncherStore

    var body: some View {
        NavigationSplitView {
            List(selection: $store.section) {
                Section("AnyPS5") {
                    ForEach(LauncherStore.Section.allCases) { section in
                        Label(section.rawValue, systemImage: section.icon).tag(section)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 180, ideal: 205)
            .safeAreaInset(edge: .bottom) {
                VStack(alignment: .leading, spacing: 5) {
                    Label("Apple Silicon + Metal", systemImage: "apple.logo")
                        .font(.caption.weight(.medium))
                    Text("Game runtime in development").font(.caption2).foregroundStyle(.secondary)
                }.padding().frame(maxWidth: .infinity, alignment: .leading)
            }
        } detail: {
            VStack(spacing: 0) {
                switch store.section {
                case .catalogue: CatalogueView(store: store)
                case .library: LibraryView(store: store)
                case .engine: EngineSettingsView(store: store)
                }
                Divider()
                HStack(spacing: 10) {
                    Circle().fill(store.isRunning ? Color.green : Color.secondary).frame(width: 7, height: 7)
                    Text(store.sessionStatus).font(.caption).lineLimit(1)
                    Spacer()
                    if store.isRunning { Button("Stop") { store.stop() }.controlSize(.small) }
                    Button { store.showConsole.toggle() } label: { Label("Console", systemImage: "terminal") }
                        .controlSize(.small)
                }.padding(.horizontal, 16).padding(.vertical, 9)
                if store.showConsole { ConsoleView(text: store.console).frame(height: 180) }
            }
        }
        .toolbar {
            ToolbarItemGroup {
                Button { store.attach() } label: { Label("Add Local Game", systemImage: "plus") }
                Button { Task { await store.refresh() } } label: { Label("Refresh Catalogue", systemImage: "arrow.clockwise") }
                    .disabled(store.isRefreshing)
            }
        }
        .alert("AnyPS5", isPresented: Binding(get: { store.error != nil }, set: { if !$0 { store.error = nil } })) {
            Button("OK") { store.error = nil }
        } message: { Text(store.error ?? "") }
    }
}

struct ConsoleView: View {
    let text: String
    var body: some View {
        ScrollView([.vertical, .horizontal]) {
            Text(text).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .topLeading).padding(12)
        }.background(.background.secondary)
    }
}
