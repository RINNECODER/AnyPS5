import LauncherCore
import SwiftUI

struct GameDetailView: View {
    @Bindable var store: LauncherStore
    let game: CatalogueGame

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                GameArtwork(url: game.primary.coverURL).frame(height: 240).clipShape(RoundedRectangle(cornerRadius: 14))
                VStack(alignment: .leading, spacing: 7) {
                    Text(game.title).font(.title2.bold())
                    Text(game.primary.publisher ?? "PS5 game").foregroundStyle(.secondary)
                    Text(game.titleIDs).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
                if let description = game.primary.description, !description.isEmpty { Text(description).font(.callout) }
                if let date = game.primary.releaseDate { LabeledContent("Released", value: date).font(.caption) }
                Divider()
                LocalGameActions(store: store, game: store.library.games.first { $0.id == game.id }, catalogueGame: game)
                Divider()
                Text("Orbit releases").font(.headline)
                Text("These console containers need a supported extraction path before AnyPS5 can load their executables.")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(game.releases) { release in
                    VStack(alignment: .leading, spacing: 7) {
                        HStack {
                            Text(release.format).font(.subheadline.weight(.semibold))
                            Spacer()
                            Text(ByteCountFormatter.string(fromByteCount: release.sizeBytes, countStyle: .file)).font(.caption)
                        }
                        Text("\(release.provider) · \(release.titleId)\(release.version.map { " · v\($0)" } ?? "")")
                            .font(.caption).foregroundStyle(.secondary)
                        if let url = release.sourceURL { Link("Open source", destination: url).font(.caption) }
                    }.padding(12).background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
                }
                Link("Orbit Store project", destination: URL(string: "https://github.com/saawant12/orbit-store-ps5")!)
                    .font(.caption)
            }.padding(22)
        }
    }
}

struct LocalGameActions: View {
    @Bindable var store: LauncherStore
    let game: LocalGame?
    var catalogueGame: CatalogueGame? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Local executable").font(.headline)
            if let game {
                Text(game.executablePath).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                Button { store.launch(game) } label: { Label("Run with AnyPS5", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent).disabled(store.isRunning || store.library.enginePath.isEmpty)
                if store.library.enginePath.isEmpty {
                    Button("Choose engine…") { store.chooseEngine() }.font(.caption)
                }
                Button("Change executable…") { store.attach(to: catalogueGame, replacing: game) }.font(.caption)
                Button("Choose resource folder…") { store.chooseResources(for: game) }.font(.caption)
                Text("Resources: \(game.workingDirectory)").font(.caption2).foregroundStyle(.secondary).textSelection(.enabled)
            } else {
                Text("Attach a clean local ELF to try it with the configured engine.").font(.callout).foregroundStyle(.secondary)
                Button("Attach local executable…") { store.attach(to: catalogueGame) }.buttonStyle(.borderedProminent)
            }
            Text("Runtime support is in development. A catalogue entry does not establish game compatibility.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}
