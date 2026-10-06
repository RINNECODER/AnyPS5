import LauncherCore
import SwiftUI

struct ConsoleHomeView: View {
    @Bindable var store: LauncherStore
    let query: String
    let isPreview: Bool
    let showOptions: (ConsoleGame) -> Void
    @FocusState private var shelfFocused: Bool

    private var games: [ConsoleGame] {
        ConsoleLibrary.games(localGames: store.library.games, catalogueGames: store.games)
            .filter { query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) }
    }
    private var selectedID: String? { ConsoleLibrary.selectedID(store.selectedID, in: games) }
    private var selected: ConsoleGame? { games.first { $0.id == selectedID } }
    private var canPlay: Bool {
        !isPreview && selected != nil && !store.isRunning && !store.isInspectingGame
            && !store.isProbingEngine && !store.isCleaningResources && !store.library.enginePath.isEmpty
    }

    var body: some View {
        GeometryReader { geometry in
            VStack(alignment: .leading, spacing: 0) {
                shelf
                Spacer(minLength: 28)
                if let game = selected {
                    VStack(alignment: .leading, spacing: 22) {
                        Text(game.title)
                            .font(.system(size: min(geometry.size.width * 0.046, 66), weight: .regular, design: .serif))
                            .lineLimit(2).shadow(color: .black.opacity(0.6), radius: 12, y: 3)
                        Text(game.genre ?? "Local game").font(.title3).foregroundStyle(.white.opacity(0.8))
                        HStack(spacing: 20) {
                            Button { store.launch(game.local) } label: {
                                Label("Play", systemImage: "play.fill").frame(minWidth: 150)
                            }
                            .buttonStyle(ConsolePillStyle(primary: true))
                            .disabled(!canPlay)
                            .opacity(canPlay || isPreview ? 1 : 0.55)
                            .accessibilityLabel("Play \(game.title)")
                            Button { showOptions(game) } label: {
                                Image(systemName: "ellipsis").font(.title2).frame(width: 54, height: 54)
                            }
                            .buttonStyle(.plain)
                            .background(.ultraThinMaterial, in: Circle())
                            .overlay { Circle().stroke(.white.opacity(0.3), lineWidth: 1) }
                            .accessibilityLabel("Options for \(game.title)")
                        }.padding(.top, 8)
                        Text(isPreview ? "Interface preview · game launch is disabled" : "Runtime in development · compatibility unverified")
                            .font(.caption).foregroundStyle(.white.opacity(0.65))
                    }
                    .frame(maxWidth: geometry.size.width * 0.67, alignment: .leading)
                    .padding(.horizontal, 54)
                } else {
                    VStack(alignment: .leading, spacing: 20) {
                        Text(query.isEmpty ? "Your next game starts here." : "No matching local games.")
                            .font(.system(size: 44, weight: .medium))
                        Text(query.isEmpty ? "Add a local game, or explore the Orbit catalogue." : "Try another search or clear the filter.")
                            .font(.title3).foregroundStyle(.secondary)
                        if query.isEmpty {
                            Button { store.attach() } label: { Label("Add local game", systemImage: "plus") }
                                .buttonStyle(ConsolePillStyle(primary: true)).disabled(isPreview || store.isRunning)
                        }
                    }.padding(.horizontal, 54)
                }
                Spacer(minLength: 34)
                HStack(spacing: 24) {
                    Label("Browse", systemImage: "arrow.left.arrow.right")
                    Label("Select", systemImage: "return")
                    Label("Options", systemImage: "ellipsis.circle")
                    Spacer()
                    if isPreview { Text("UI preview").foregroundStyle(ConsoleTheme.blue) }
                }
                .font(.callout).foregroundStyle(.white.opacity(0.75))
                .padding(.horizontal, 54).padding(.bottom, 24)
            }
        }
        .background {
            GeometryReader { geometry in
                ConsoleArtwork(url: selected?.heroURL, title: selected?.title ?? "MacPS", hero: true, conceptHero: isPreview)
                    .frame(width: geometry.size.width, height: geometry.size.height).clipped()
                    .overlay {
                        LinearGradient(stops: [.init(color: .black.opacity(0.62), location: 0), .init(color: .black.opacity(0.22), location: 0.65), .init(color: .clear, location: 1)], startPoint: .leading, endPoint: .trailing)
                    }
                    .overlay { LinearGradient(colors: [.black.opacity(0.15), .clear, .black.opacity(0.65)], startPoint: .top, endPoint: .bottom) }
            }.ignoresSafeArea()
        }
        .onChange(of: games.map(\.id), initial: true) { _, _ in
            store.selectedID = ConsoleLibrary.selectedID(store.selectedID, in: games)
        }
    }

    private var shelf: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(alignment: .top, spacing: 18) {
                    ForEach(games) { game in
                        ConsoleShelfTile(game: game, selected: game.id == selectedID) {
                            store.selectedID = game.id
                        }.id(game.id)
                    }
                    Button { store.attach() } label: {
                        VStack(spacing: 14) {
                            Image(systemName: "plus").font(.system(size: 32, weight: .light))
                            Text("Add game").font(.callout)
                        }
                        .frame(width: 126, height: 136)
                        .background(.white.opacity(0.10), in: RoundedRectangle(cornerRadius: 18))
                        .overlay { RoundedRectangle(cornerRadius: 18).stroke(.white.opacity(0.15), lineWidth: 1) }
                    }.buttonStyle(.plain).disabled(isPreview || store.isRunning)
                }
                .padding(.horizontal, 54).padding(.top, 22).padding(.bottom, 20)
            }
            .focusable().focusEffectDisabled().focused($shelfFocused)
            .onAppear { shelfFocused = true }
            .onKeyPress(.leftArrow) { move(forward: false); return .handled }
            .onKeyPress(.rightArrow) { move(forward: true); return .handled }
            .onKeyPress(.return) {
                if canPlay, let game = selected { store.launch(game.local); return .handled }
                return .ignored
            }
            .onChange(of: selectedID) { _, id in
                if let id { withAnimation(.easeOut(duration: 0.22)) { proxy.scrollTo(id, anchor: .leading) } }
            }
        }
    }

    private func move(forward: Bool) {
        store.selectedID = forward ? ConsoleLibrary.nextID(after: selectedID, in: games)
            : ConsoleLibrary.previousID(before: selectedID, in: games)
    }
}

private struct ConsoleShelfTile: View {
    let game: ConsoleGame
    let selected: Bool
    let select: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: select) {
            ConsoleArtwork(url: game.coverURL, title: game.title)
                .frame(width: selected ? 176 : 126, height: selected ? 176 : 136)
                .clipShape(RoundedRectangle(cornerRadius: 18))
                .overlay { RoundedRectangle(cornerRadius: 18).stroke(selected ? .white : .white.opacity(0.22), lineWidth: selected ? 3 : 1) }
                .shadow(color: selected ? ConsoleTheme.blue.opacity(0.85) : .clear, radius: 12)
                .overlay(alignment: .bottomLeading) {
                    if game.coverURL == nil {
                        Text(game.title).font(.caption.weight(.medium)).lineLimit(2).padding(12)
                    }
                }
                .padding(.top, selected ? 0 : 9)
        }
        .buttonStyle(.plain).scaleEffect(hovered ? 1.035 : 1)
        .onHover { hovered = $0 }
        .animation(.easeOut(duration: 0.18), value: selected)
        .accessibilityLabel(game.title)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}
