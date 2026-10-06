import SwiftUI

enum ConsoleTheme {
    static let blue = Color(red: 0.30, green: 0.62, blue: 1)
    static let ink = Color(red: 0.025, green: 0.045, blue: 0.075)
}

struct MPSWordmark: View {
    var body: some View {
        GeometryReader { geometry in
            Path { path in
                let w = geometry.size.width / 100
                let h = geometry.size.height / 24
                path.move(to: CGPoint(x: 2*w, y: 22*h))
                path.addLine(to: CGPoint(x: 2*w, y: 3*h))
                path.addQuadCurve(to: CGPoint(x: 5*w, y: 2*h), control: CGPoint(x: 2*w, y: 0))
                path.addLine(to: CGPoint(x: 17*w, y: 16*h))
                path.addLine(to: CGPoint(x: 29*w, y: 2*h))
                path.addQuadCurve(to: CGPoint(x: 32*w, y: 3*h), control: CGPoint(x: 32*w, y: 0))
                path.addLine(to: CGPoint(x: 32*w, y: 22*h))
                path.move(to: CGPoint(x: 42*w, y: 2*h))
                path.addLine(to: CGPoint(x: 60*w, y: 2*h))
                path.addCurve(to: CGPoint(x: 60*w, y: 13*h), control1: CGPoint(x: 70*w, y: 2*h), control2: CGPoint(x: 70*w, y: 13*h))
                path.addLine(to: CGPoint(x: 46*w, y: 13*h))
                path.addQuadCurve(to: CGPoint(x: 42*w, y: 17*h), control: CGPoint(x: 42*w, y: 13*h))
                path.addLine(to: CGPoint(x: 42*w, y: 22*h))
                path.move(to: CGPoint(x: 98*w, y: 2*h))
                path.addLine(to: CGPoint(x: 80*w, y: 2*h))
                path.addCurve(to: CGPoint(x: 80*w, y: 12*h), control1: CGPoint(x: 70*w, y: 2*h), control2: CGPoint(x: 70*w, y: 12*h))
                path.addLine(to: CGPoint(x: 91*w, y: 12*h))
                path.addCurve(to: CGPoint(x: 91*w, y: 22*h), control1: CGPoint(x: 101*w, y: 12*h), control2: CGPoint(x: 101*w, y: 22*h))
                path.addLine(to: CGPoint(x: 74*w, y: 22*h))
            }
            .stroke(.white, style: StrokeStyle(lineWidth: geometry.size.height * 0.13, lineCap: .round, lineJoin: .round))
        }
        .accessibilityLabel("MacPS")
    }
}

struct ConsoleArtwork: View {
    let url: URL?
    let title: String
    var hero = false
    var conceptHero = false

    var body: some View {
        if hero && conceptHero && title.localizedCaseInsensitiveContains("civilization") {
            Image("ConsoleCoast", bundle: .module).resizable().scaledToFill()
        } else {
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
