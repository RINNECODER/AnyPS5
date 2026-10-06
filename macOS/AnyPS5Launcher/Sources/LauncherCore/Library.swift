import Foundation

public struct LocalGame: Codable, Identifiable, Sendable, Equatable {
    public let id: String
    public var title: String
    public var executablePath: String
    public var workingDirectory: String
    public var resourceImagePath: String?
    public var sceModulePaths: [String]

    public init(id: String, title: String, executablePath: String, workingDirectory: String, resourceImagePath: String? = nil,
                sceModulePaths: [String] = []) {
        self.id = id
        self.title = title
        self.executablePath = executablePath
        self.workingDirectory = workingDirectory
        self.resourceImagePath = resourceImagePath
        self.sceModulePaths = sceModulePaths
    }

    enum CodingKeys: String, CodingKey {
        case id, title, executablePath, workingDirectory, resourceImagePath, sceModulePaths
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(String.self, forKey: .id)
        title = try values.decode(String.self, forKey: .title)
        executablePath = try values.decode(String.self, forKey: .executablePath)
        workingDirectory = try values.decode(String.self, forKey: .workingDirectory)
        resourceImagePath = try values.decodeIfPresent(String.self, forKey: .resourceImagePath)
        sceModulePaths = try values.decodeIfPresent([String].self, forKey: .sceModulePaths) ?? []
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
