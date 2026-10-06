import SwiftUI

enum ConsoleTheme {
    static let blue = Color(red: 0.30, green: 0.62, blue: 1)
    static let ink = Color(red: 0.025, green: 0.045, blue: 0.075)
}

struct ConsoleArtwork: View {
    let url: URL?
    let title: String
    var hero = false

    var body: some View {
        AsyncImage(url: url) { phase in
            if let image = phase.image {
                image.resizable().scaledToFill()
            } else {
                ZStack {
                    LinearGradient(colors: [ConsoleTheme.blue.opacity(0.35), ConsoleTheme.ink], startPoint: .topLeading, endPoint: .bottomTrailing)
                    Image(systemName: "gamecontroller").font(.system(size: hero ? 90 : 38, weight: .ultraLight)).foregroundStyle(.white.opacity(0.3))
                }
            }
        }
    }
}

struct ConsolePillStyle: ButtonStyle {
    var primary = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 19, weight: .medium))
            .padding(.horizontal, 38).padding(.vertical, 17)
            .foregroundStyle(primary ? Color.black : Color.white)
            .background(primary ? Color.white.opacity(configuration.isPressed ? 0.75 : 0.95) : Color.white.opacity(configuration.isPressed ? 0.2 : 0.1), in: Capsule())
            .overlay { Capsule().stroke(.white.opacity(primary ? 0.8 : 0.3), lineWidth: 1) }
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
    }
}
