import Foundation

public struct LocalGame: Codable, Identifiable, Sendable, Equatable {
    public let id: String
    public var title: String
    public var executablePath: String
    public var workingDirectory: String
    public var resourceImagePath: String?

    public init(id: String, title: String, executablePath: String, workingDirectory: String, resourceImagePath: String? = nil) {
        self.id = id
        self.title = title
        self.executablePath = executablePath
        self.workingDirectory = workingDirectory
        self.resourceImagePath = resourceImagePath
    }
}

public struct LauncherLibrary: Codable, Sendable {
    public var games: [LocalGame] = []
    public var enginePath: String = ""
    public init() {}

    public mutating func attach(_ game: LocalGame) {
        if let index = games.firstIndex(where: { $0.id == game.id }) { games[index] = game }
        else { games.append(game) }
    }
}

public struct LibraryPersistence {
    private let url: URL
    public init(url: URL) { self.url = url }

    public func load() throws -> LauncherLibrary {
        guard FileManager.default.fileExists(atPath: url.path) else { return LauncherLibrary() }
        return try JSONDecoder().decode(LauncherLibrary.self, from: Data(contentsOf: url))
    }

    public func save(_ library: LauncherLibrary) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(library).write(to: url, options: .atomic)
    }
}
