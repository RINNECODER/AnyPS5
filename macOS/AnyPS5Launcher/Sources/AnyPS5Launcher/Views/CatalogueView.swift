import LauncherCore
import SwiftUI

struct CatalogueView: View {
    @Bindable var store: LauncherStore
    private let columns = [GridItem(.adaptive(minimum: 165, maximum: 225), spacing: 20)]

    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 0) {
                VStack(alignment: .leading, spacing: 7) {
                    Text("Orbit Catalogue").font(.largeTitle.weight(.bold))
                    Text("Find a game. Connect your local copy. Run with AnyPS5.").foregroundStyle(.secondary)
                    if let snapshot = store.snapshot {
                        Text("\(store.games.count) games · \(snapshot.catalogue.releases.count) releases · \(snapshot.isCached ? "Saved" : "Updated") \(snapshot.fetchedAt.formatted(date: .abbreviated, time: .shortened))")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if let warning = store.snapshot?.warning ?? store.catalogueError {
                        Label(warning, systemImage: "wifi.exclamationmark").font(.caption).foregroundStyle(.orange)
                    }
                }.frame(maxWidth: .infinity, alignment: .leading).padding(24)
                if store.games.isEmpty {
                    ContentUnavailableView {
                        Label(store.isRefreshing ? "Loading Orbit" : "Catalogue unavailable", systemImage: "square.grid.2x2")
                    } description: { Text(store.catalogueError ?? "Fetching the catalogue from Orbit Store.") }
                    actions: { Button("Refresh") { Task { await store.refresh() } }.disabled(store.isRefreshing) }
                } else if store.visibleGames.isEmpty {
                    ContentUnavailableView.search(text: store.search)
                } else {
                    ScrollView {
                        LazyVGrid(columns: columns, spacing: 24) {
                            ForEach(store.visibleGames) { game in
                                Button { store.selectedID = game.id } label: {
                                    GameCard(game: game, isSelected: store.selectedID == game.id,
                                             isLocal: store.library.games.contains { $0.id == game.id })
                                }.buttonStyle(.plain)
                            }
                        }.padding(.horizontal, 24).padding(.bottom, 24)
                    }
                }
            }.frame(minWidth: 420, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .searchable(text: $store.search, prompt: "Search games or title IDs")
            if let game = store.selectedGame {
                GameDetailView(store: store, game: game).frame(minWidth: 310, idealWidth: 350, maxWidth: 390)
            }
        }
    }
}

struct GameCard: View {
    let game: CatalogueGame
    let isSelected: Bool
    let isLocal: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            GameArtwork(url: game.primary.coverURL)
                .aspectRatio(1, contentMode: .fit).clipShape(RoundedRectangle(cornerRadius: 12))
                .overlay { RoundedRectangle(cornerRadius: 12).stroke(isSelected ? Color.accentColor : .clear, lineWidth: 3) }
                .overlay(alignment: .bottomLeading) {
                    if isLocal {
                        Label("Local copy", systemImage: "externaldrive").font(.caption2.weight(.semibold))
                            .padding(7).background(.regularMaterial, in: Capsule()).padding(8)
                    }
                }
            Text(game.title).font(.headline).lineLimit(2)
                .frame(maxWidth: .infinity, minHeight: 36, maxHeight: 36, alignment: .topLeading).clipped()
            Text(game.primary.genre ?? "PS5").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading).clipped()
        }.frame(maxWidth: .infinity, alignment: .leading)
            .clipped().contentShape(Rectangle()).accessibilityLabel(game.title)
    }
}

struct GameArtwork: View {
    let url: URL?
    var body: some View {
        AsyncImage(url: url) { image in image.resizable().scaledToFill() } placeholder: {
            ZStack {
                Rectangle().fill(.quaternary)
                Image(systemName: "gamecontroller").font(.system(size: 42)).foregroundStyle(.secondary)
            }
        }.frame(maxWidth: .infinity, maxHeight: .infinity).clipped()
    }
}
