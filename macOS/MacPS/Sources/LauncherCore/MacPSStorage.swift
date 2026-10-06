import Foundation

/// Moves app configuration to the MacPS identity without moving game or engine files.
public enum MacPSStorage {
    public static func prepare(in applicationSupport: URL) throws -> URL {
        let current = applicationSupport.appendingPathComponent("MacPS", isDirectory: true)
        let legacy = applicationSupport.appendingPathComponent("AnyPS5Launcher", isDirectory: true)
        let currentLibrary = current.appendingPathComponent("library.json")
        let legacyLibrary = legacy.appendingPathComponent("library.json")
        let files = FileManager.default
        if !files.fileExists(atPath: currentLibrary.path), files.fileExists(atPath: legacyLibrary.path) {
            do {
                let data = try Data(contentsOf: legacyLibrary)
                _ = try JSONDecoder().decode(LauncherLibrary.self, from: data)
                try files.createDirectory(at: current, withIntermediateDirectories: true)
                try data.write(to: currentLibrary, options: .atomic)
            } catch {
                throw LauncherError("The saved library at \(legacyLibrary.path) could not be migrated. The original has been preserved. \(error.localizedDescription)")
            }
        }
        let currentCache = current.appendingPathComponent("orbit-catalogue-v2.json")
        let legacyCache = legacy.appendingPathComponent("orbit-catalogue-v2.json")
        if !files.fileExists(atPath: currentCache.path), files.fileExists(atPath: legacyCache.path) {
            // The cache is optional; a failed copy can be recovered with a catalogue refresh.
            try? files.createDirectory(at: current, withIntermediateDirectories: true)
            try? files.copyItem(at: legacyCache, to: currentCache)
        }
        return current
    }
}
