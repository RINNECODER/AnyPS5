import LauncherCore
import SwiftUI

/// A catalogue browser with view-local filters: browsing Orbit never changes library selection.
struct OrbitStoreView: View {
    @Bindable var store: LauncherStore
    @State private var query = ""
    @State private var genre = "All genres"
    @State private var detailGame: CatalogueGame?

    private let columns = [GridItem(.adaptive(minimum: 210, maximum: 280), spacing: 24)]
    private let accent = Color(red: 0.24, green: 0.59, blue: 1)

    private var genres: [String] {
        ["All genres"] + Set(store.games.compactMap { game in
            let value = game.primary.genre?.trimmingCharacters(in: .whitespacesAndNewlines)
            return value?.isEmpty == false ? value : nil
        }).sorted { $0.localizedStandardCompare($1) == .orderedAscending }
    }

    private var filteredGames: [CatalogueGame] {
        let search = query.trimmingCharacters(in: .whitespacesAndNewlines)
        return store.games.filter { game in
            let matchesQuery = search.isEmpty || game.title.localizedCaseInsensitiveContains(search)
                || game.titleIDs.localizedCaseInsensitiveContains(search)
                || (game.primary.publisher?.localizedCaseInsensitiveContains(search) ?? false)
            let matchesGenre = genre == "All genres"
                || game.primary.genre?.trimmingCharacters(in: .whitespacesAndNewlines) == genre
            return matchesQuery && matchesGenre
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 30) {
                header
                catalogueStatus
                if store.games.isEmpty {
                    unavailableCatalogue
                } else if filteredGames.isEmpty {
                    ContentUnavailableView {
                        Label("No matching games", systemImage: "magnifyingglass")
                    } description: {
                        Text("Try a different title, publisher, title ID, or genre.")
                    } actions: {
                        Button("Clear filters") { query = ""; genre = "All genres" }
                    }
                    .frame(maxWidth: .infinity, minHeight: 300)
                } else {
                    if let featured = filteredGames.first {
                        OrbitFeaturedGame(game: featured, isLocal: isLocal(featured)) {
                            detailGame = featured
                        }
                    }
                    HStack {
                        Text(query.isEmpty && genre == "All genres" ? "Explore the catalogue" : "Matching games")
                            .font(.title2.weight(.semibold))
                        Spacer()
                        Text("\(filteredGames.count) games")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    LazyVGrid(columns: columns, spacing: 28) {
                        ForEach(filteredGames) { game in
                            OrbitGameTile(game: game, isLocal: isLocal(game)) {
                                detailGame = game
                            }
                        }
                    }
                }
            }
            .padding(.horizontal, 46)
            .padding(.top, 22)
            .padding(.bottom, 46)
        }
        .background {
            LinearGradient(colors: [Color(red: 0.07, green: 0.11, blue: 0.19), Color.black.opacity(0.85)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
        }
        .tint(accent)
        .preferredColorScheme(.dark)
        .sheet(item: $detailGame) { game in
            VStack(spacing: 0) {
                HStack {
                    Label("Catalogue", systemImage: "sparkles").font(.headline)
                    Spacer()
                    Button("Done") { detailGame = nil }.keyboardShortcut(.cancelAction)
                }.padding(20)
                Divider()
                GameDetailView(store: store, game: game)

            }
            .frame(minWidth: 640, idealWidth: 760, minHeight: 620, idealHeight: 760)
            .preferredColorScheme(.dark)
        }
        .onChange(of: genres) { _, updated in
            if !updated.contains(genre) { genre = "All genres" }
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Catalogue").font(.system(size: 38, weight: .semibold))
                    Text("Browse games and add your local copy to the library.")
                        .font(.title3).foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    Task { await store.refresh() }
                } label: {
                    Label(store.isRefreshing ? "Refreshing…" : "Refresh catalogue", systemImage: "arrow.clockwise")
                        .padding(.vertical, 7)
                }
                .buttonStyle(.bordered)
                .disabled(store.isRefreshing)
            }
            HStack(spacing: 22) {
                HStack(spacing: 12) {
                    Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                    TextField("Search titles, publishers, or title IDs", text: $query)
                        .textFieldStyle(.plain)
                        .accessibilityLabel("Search Orbit Store")
                    if !query.isEmpty {
                        Button { query = "" } label: { Image(systemName: "xmark.circle.fill") }
                            .buttonStyle(.plain).accessibilityLabel("Clear Orbit search")
                    }
                }
                .padding(15)
                .background(.white.opacity(0.07), in: RoundedRectangle(cornerRadius: 13))
                .frame(maxWidth: 520)
                Picker("Genre", selection: $genre) {
                    ForEach(genres, id: \.self) { value in Text(value).tag(value) }
                }
                .frame(width: 230)
                .accessibilityLabel("Filter Orbit games by genre")
                Spacer(minLength: 0)
            }
        }
    }

    private var catalogueStatus: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let snapshot = store.snapshot {
                Label("\(snapshot.isCached ? "Saved catalogue" : "Updated catalogue") · \(snapshot.fetchedAt.formatted(date: .abbreviated, time: .shortened))",
                      systemImage: snapshot.isCached ? "externaldrive" : "checkmark.circle")
                    .font(.callout).foregroundStyle(.secondary)
            }
            if let warning = store.catalogueError ?? store.snapshot?.warning {
                Label(warning, systemImage: "wifi.exclamationmark")
                    .font(.callout).foregroundStyle(.orange).textSelection(.enabled)
            }
            Text("Catalogue listings include source links. Attach a local executable to add a game to your library; runtime support is still in development.")
                .font(.callout).foregroundStyle(.secondary)
        }
    }

    private var unavailableCatalogue: some View {
        ContentUnavailableView {
            if store.isRefreshing {
                Label("Loading Orbit", systemImage: "sparkles")
            } else {
                Label("Catalogue unavailable", systemImage: "square.grid.2x2")
            }
        } description: {
            Text(store.catalogueError ?? "Refresh to retrieve the Orbit catalogue.")
        } actions: {
            if store.isRefreshing {
                ProgressView().controlSize(.large)
            } else {
                Button("Try again") { Task { await store.refresh() } }.buttonStyle(.borderedProminent)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 340)
    }

    private func isLocal(_ game: CatalogueGame) -> Bool {
        store.library.games.contains { $0.id == game.id }
    }
}

private struct OrbitFeaturedGame: View {
    let game: CatalogueGame
    let isLocal: Bool
    let showDetails: () -> Void

    var body: some View {
        HStack(spacing: 32) {
            OrbitCoverArtwork(url: game.primary.coverURL)
                .frame(width: 216, height: 216)
                .clipShape(RoundedRectangle(cornerRadius: 18))
            VStack(alignment: .leading, spacing: 14) {
                Text("IN THE CATALOGUE").font(.caption.weight(.bold)).tracking(3).foregroundStyle(.secondary)
                Text(game.title).font(.system(size: 32, weight: .semibold)).lineLimit(2)
                Text([game.primary.genre, game.primary.publisher].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.title3).foregroundStyle(.secondary)
                if isLocal {
                    Label("Local copy attached", systemImage: "externaldrive")
                        .font(.callout).foregroundStyle(.secondary)
                }
                Button(action: showDetails) {
                    Label("View game", systemImage: "arrow.right").padding(.horizontal, 18).padding(.vertical, 9)
                }
                .buttonStyle(.borderedProminent)
                .accessibilityLabel("View \(game.title) details")
            }
            Spacer(minLength: 0)
        }
        .padding(30)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            RoundedRectangle(cornerRadius: 24)
                .fill(LinearGradient(colors: [.blue.opacity(0.20), .white.opacity(0.04)],
                                     startPoint: .topLeading, endPoint: .bottomTrailing))
        }
        .overlay { RoundedRectangle(cornerRadius: 24).stroke(.white.opacity(0.10), lineWidth: 1) }
    }
}

private struct OrbitGameTile: View {
    let game: CatalogueGame
    let isLocal: Bool
    let showDetails: () -> Void
    @FocusState private var focused: Bool
    @State private var hovered = false

    var body: some View {
        Button(action: showDetails) {
            VStack(alignment: .leading, spacing: 12) {
                OrbitCoverArtwork(url: game.primary.coverURL)
                    .aspectRatio(1, contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 17))
                    .overlay(alignment: .bottomLeading) {
                        if isLocal {
                            Label("Local copy", systemImage: "externaldrive")
                                .font(.caption.weight(.semibold)).padding(9)
                                .background(.ultraThinMaterial, in: Capsule()).padding(12)
                        }
                    }
                    .overlay {
                        RoundedRectangle(cornerRadius: 17)
                            .stroke(focused || hovered ? Color.accentColor : .white.opacity(0.10),
                                    lineWidth: focused ? 3 : 1)
                    }
                Text(game.title).font(.headline).lineLimit(2)
                    .frame(maxWidth: .infinity, minHeight: 42, alignment: .topLeading)
                Text(game.primary.genre ?? "Game").font(.callout).foregroundStyle(.secondary).lineLimit(1)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .focused($focused)
        .onHover { hovered = $0 }
        .accessibilityLabel("\(game.title)\(isLocal ? ", local copy attached" : "")")
        .accessibilityHint("Open game details and source links")
    }
}

private struct OrbitCoverArtwork: View {
    let url: URL?

    var body: some View {
        AsyncImage(url: url) { phase in
            if let image = phase.image {
                image.resizable().scaledToFill()
            } else {
                ZStack {
                    LinearGradient(colors: [.blue.opacity(0.25), .white.opacity(0.05)],
                                   startPoint: .topLeading, endPoint: .bottomTrailing)
                    Image(systemName: "gamecontroller")
                        .font(.system(size: 42, weight: .light)).foregroundStyle(.secondary)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .clipped()
        .accessibilityHidden(true)
    }
}
