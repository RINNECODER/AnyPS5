import AppKit
import LauncherCore
import SwiftUI

struct ConsoleRootView: View {
    @Bindable var store: LauncherStore
    let isPreview: Bool
    @State private var showingStore = false
    @State private var showingSearch = false
    @State private var showingSettings = false
    @State private var query = ""
    @State private var optionsGame: ConsoleGame?
    @FocusState private var searchFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            header
            if showingStore {
                OrbitStoreView(store: store, isPreview: isPreview)
            } else {
                ConsoleHomeView(store: store, query: query, isPreview: isPreview) { optionsGame = $0 }
            }
            if !isPreview && (store.isRunning || store.showConsole || store.hasMountedResources) {
                sessionBar
                if store.showConsole { ConsoleView(text: store.console).frame(height: 160) }
            }
        }
        .background(ConsoleTheme.ink).foregroundStyle(.white).tint(ConsoleTheme.blue)
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showingSettings) {
            VStack(spacing: 0) {
                sheetHeader("Settings") { showingSettings = false }
                EngineSettingsView(store: store).disabled(isPreview)
            }.frame(width: 800, height: 700).preferredColorScheme(.dark)
        }
        .sheet(item: $optionsGame) { game in
            VStack(spacing: 0) {
                sheetHeader(game.title) { optionsGame = nil }
                ScrollView {
                    LocalGameActions(store: store,
                                     game: store.library.games.first { $0.id == game.id },
                                     catalogueGame: game.catalogue)
                        .padding(28).disabled(isPreview)
                }
            }.frame(width: 640, height: 680).preferredColorScheme(.dark)
        }
        .alert("MacPS", isPresented: Binding(get: { store.error != nil }, set: { if !$0 { store.error = nil } })) {
            Button("OK") { store.error = nil }
        } message: { Text(store.error ?? "") }
    }

    private var header: some View {
        HStack(spacing: 30) {
            MPSWordmark().frame(width: 88, height: 24)
            HStack(spacing: 30) {
                tab("Games", selected: !showingStore) { showingStore = false }
                tab("Orbit Store", selected: showingStore) { showingStore = true; showingSearch = false; query = "" }
            }.padding(.leading, 18)
            Spacer(minLength: 8)
            if showingSearch && !showingStore {
                HStack(spacing: 10) {
                    TextField("Search your library", text: $query)
                        .textFieldStyle(.plain).focused($searchFocused)
                        .accessibilityLabel("Search local library")
                    Button { query = ""; showingSearch = false } label: { Image(systemName: "xmark") }
                        .buttonStyle(.plain).accessibilityLabel("Close library search")
                }.padding(10).frame(width: 235).background(.white.opacity(0.09), in: Capsule())
            }
            if !showingStore {
                iconButton("magnifyingglass", label: "Search library") {
                    showingSearch.toggle(); searchFocused = showingSearch
                    if !showingSearch { query = "" }
                }
            }
            iconButton("gearshape", label: "Settings") { showingSettings = true }
            iconButton("arrow.up.left.and.arrow.down.right", label: "Toggle Full Screen") { NSApp.keyWindow?.toggleFullScreen(nil) }
            TimelineView(.periodic(from: .now, by: 60)) { context in
                Text(context.date.formatted(date: .omitted, time: .shortened))
                    .font(.system(size: 18, weight: .light)).foregroundStyle(.white.opacity(0.8))
            }.frame(minWidth: 74, alignment: .trailing)
        }
        .padding(.horizontal, 54).padding(.vertical, 25)
        .background(ConsoleTheme.ink.opacity(0.72))
    }

    private func tab(_ title: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title).font(.system(size: 23, weight: selected ? .semibold : .regular))
                .foregroundStyle(.white.opacity(selected ? 1 : 0.6))
                .padding(.vertical, 7)
                .overlay(alignment: .bottom) {
                    if selected { Capsule().fill(ConsoleTheme.blue).frame(height: 3).offset(y: 5).shadow(color: ConsoleTheme.blue, radius: 6) }
                }
        }.buttonStyle(.plain).accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func iconButton(_ icon: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: icon).font(.system(size: 20)).frame(width: 32, height: 36) }
            .buttonStyle(.plain).help(label).accessibilityLabel(label)
    }

    private func sheetHeader(_ title: String, close: @escaping () -> Void) -> some View {
        HStack {
            Text(title).font(.title3.weight(.semibold))
            Spacer()
            Button("Done", action: close).keyboardShortcut(.cancelAction)
        }.padding(22).background(.white.opacity(0.04))
    }

    private var sessionBar: some View {
        HStack(spacing: 12) {
            Circle().fill(store.isRunning ? ConsoleTheme.blue : .gray).frame(width: 7, height: 7)
            Text(store.sessionStatus).font(.callout).lineLimit(1)
            Spacer()
            if store.isRunning { Button("Stop session") { store.stop() }.buttonStyle(.bordered) }
            Button(store.showConsole ? "Hide activity" : "Show activity") { store.showConsole.toggle() }.buttonStyle(.bordered)
        }.padding(.horizontal, 28).padding(.vertical, 12).background(ConsoleTheme.ink)
    }
}
