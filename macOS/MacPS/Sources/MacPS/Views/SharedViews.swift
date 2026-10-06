import SwiftUI

struct ConsoleView: View {
    let text: String
    var body: some View {
        ScrollView([.vertical, .horizontal]) {
            Text(text).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .topLeading).padding(12)
        }.background(.background.secondary)
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
