import SwiftUI

struct LibraryView: View {
    @Bindable var store: LauncherStore
    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("My Library").font(.largeTitle.bold())
                    Text("Local executables attached to AnyPS5.").foregroundStyle(.secondary)
                }.padding([.horizontal, .top], 24)
                if store.library.games.isEmpty {
                    ContentUnavailableView {
                        Label("Add your first local game", systemImage: "gamecontroller")
                    } description: { Text("Attach an executable from the catalogue, or import a homebrew ELF.") }
                    actions: { Button("Add Local Executable…") { store.attach() } }
                } else {
                    List(store.visibleLocalGames, selection: $store.selectedID) { game in
                        VStack(alignment: .leading, spacing: 5) {
                            Text(game.title).font(.headline)
                            Text(game.executablePath).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        }.padding(.vertical, 5).tag(game.id)
                    }.listStyle(.inset)
                }
            }.frame(minWidth: 400).searchable(text: $store.search, prompt: "Search local games")
            if let game = store.selectedLocal {
                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        Text(game.title).font(.title.bold())
                        LocalGameActions(store: store, game: game, catalogueGame: store.selectedGame)
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: game.executablePath)]) }
                        Button("Remove from Library") { store.removeFromLibrary(game) }
                            .disabled(store.isRunning)
                        Text("Removing a library entry keeps its files on disk.").font(.caption).foregroundStyle(.secondary)
                    }.padding(24)
                }.frame(minWidth: 310, idealWidth: 350, maxWidth: 410)
            }
        }
    }
}
