import Foundation
import LauncherCore

/// An isolated presentation run never reads or writes the live launcher's saved state.
enum MacPSPreview {
    static let enabled = Bundle.main.bundleIdentifier == "com.macps.console.preview"
        || CommandLine.arguments.contains("--macps-preview")
    static var supportDirectory: URL? {
        guard enabled else { return nil }
        if let index = CommandLine.arguments.firstIndex(of: "--preview-state"), CommandLine.arguments.indices.contains(index + 1) {
            return URL(fileURLWithPath: CommandLine.arguments[index + 1], isDirectory: true)
        }
        if let path = Bundle.main.object(forInfoDictionaryKey: "MacPSPreviewStateDirectory") as? String {
            return URL(fileURLWithPath: path, isDirectory: true)
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("MacPSPreview", isDirectory: true)
    }

    @MainActor static func loadCatalogue(into store: LauncherStore) {
        guard let directory = supportDirectory,
              let data = try? Data(contentsOf: directory.appendingPathComponent("orbit-catalogue-v2.json")),
              let catalogue = try? OrbitCatalogue.decode(data) else { return }
        store.games = catalogue.games
        store.catalogueError = "UI preview uses a saved catalogue. Local game and engine actions are disabled."
    }
}
