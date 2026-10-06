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
    @State private var showingRemovalConfirmation = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let game {
                Label("Added to your library", systemImage: "checkmark.circle").font(.headline)
                Text("Launch this game from the Library. Runtime compatibility is still in development.")
                    .font(.callout).foregroundStyle(.secondary)
                Button("Remove from library", role: .destructive) { showingRemovalConfirmation = true }
                    .disabled(store.isRunning || store.isInspectingGame || store.isCleaningResources)
                DisclosureGroup("Advanced game settings") {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("Local executable").font(.headline)
                        Text(game.executablePath).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                        Button("Inspect executable") { store.inspect(game) }
                            .disabled(store.isRunning || store.isInspectingGame || store.isProbingEngine || store.library.enginePath.isEmpty)
                        if store.inspectedGame == game, let text = store.inspectionText {
                            Text(text).font(.caption.monospaced()).textSelection(.enabled)
                        }
                        if store.library.enginePath.isEmpty {
                            Button("Choose engine…") { store.chooseEngine() }.font(.caption)
                        }
                        Button("Change executable…") { store.attach(to: catalogueGame, replacing: game) }.font(.caption)
                        Button("Choose resource folder…") { store.chooseResources(for: game) }.font(.caption)
                        Button("Use exFAT resource image…") { store.chooseImageResources(for: game) }
                            .font(.caption).disabled(store.isRunning)
                        Text(game.resourceImagePath.map { "Read-only resources: \($0)" } ?? "Resources: \(game.workingDirectory)")
                            .font(.caption2).foregroundStyle(.secondary).textSelection(.enabled)
                        Divider()
                        Text("Local modules").font(.headline)
                        Button("Add local modules…") { store.chooseModules(for: game) }
                            .font(.caption).disabled(store.isRunning || store.isInspectingGame || store.isCleaningResources)
                        if game.sceModulePaths.isEmpty {
                            Text("Attach local libraries required by this game.").font(.caption).foregroundStyle(.secondary)
                        } else {
                            ForEach(Array(game.sceModulePaths.enumerated()), id: \.offset) { _, path in
                                HStack(alignment: .top) {
                                    Text(path).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                                    Spacer()
                                    Button("Remove") { store.removeModule(path, from: game) }
                                        .font(.caption).help("Remove \(URL(fileURLWithPath: path).lastPathComponent)")
                                        .disabled(store.isRunning || store.isInspectingGame || store.isCleaningResources)
                                }
                            }
                            if store.capabilities?.sceModuleArgument != "--sce-module" {
                                Text("Choose an engine with local module loading support to run this configuration.")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        if store.hasMountedResources {
                            Button("Retry resource cleanup") { Task { await store.retryResourceCleanup() } }
                                .font(.caption).disabled(store.isRunning || store.isCleaningResources)
                        }
                    }.padding(.top, 12)
                }.padding(.top, 12)
            } else {
                Text("Add your local copy to the library. Select its supported game executable; a disk image alone cannot be launched.").font(.callout).foregroundStyle(.secondary)
                Button("Add to library…") { store.attach(to: catalogueGame) }.buttonStyle(.borderedProminent)
            }
            Text("Runtime support is in development. A catalogue entry does not establish game compatibility.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .confirmationDialog("Remove this game from your library?", isPresented: $showingRemovalConfirmation) {
            if let game {
                Button("Remove from library", role: .destructive) { store.removeFromLibrary(game) }
            }
        } message: {
            Text("Your game files and resources will stay in their original locations.")
        }
    }
}
