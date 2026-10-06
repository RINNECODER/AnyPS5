import Foundation

public struct OrbitRelease: Decodable, Identifiable, Sendable {
    public let id: String
    public let gameId: String
    public let titleId: String
    public let title: String
    public let sizeBytes: Int64
    public let provider: String
    public let format: String
    public let version: String?
    public let filename: String
    public let url: String
    public let cover: String?
    public let hero: String?
    public let coverFallback: String?
    public let publisher: String?
    public let genre: String?
    public let description: String?
    public let releaseDate: String?

    public var sourceURL: URL? { Self.webURL(url) }
    public var coverURL: URL? { Self.webURL(cover ?? "") ?? Self.webURL(coverFallback ?? "") }

    private static func webURL(_ value: String) -> URL? {
        guard let url = URL(string: value), url.scheme == "https", url.host != nil else { return nil }
        return url
    }
}

public struct CatalogueGame: Identifiable, Sendable {
    public let id: String
    public let releases: [OrbitRelease]
    public var primary: OrbitRelease { releases[0] }
    public var title: String { primary.title }
    public var titleIDs: String { Set(releases.map(\.titleId)).sorted().joined(separator: ", ") }
}

public struct OrbitCatalogue: Decodable, Sendable {
    public let schemaVersion: Int
    public let revision: Int
    public let releases: [OrbitRelease]

    public static func decode(_ data: Data) throws -> Self {
        let catalogue = try JSONDecoder().decode(Self.self, from: data)
        guard catalogue.schemaVersion == 1 else { throw LauncherError("Unsupported Orbit catalogue schema \(catalogue.schemaVersion).") }
        guard !catalogue.releases.isEmpty,
              Set(catalogue.releases.map(\.id)).count == catalogue.releases.count,
              catalogue.releases.allSatisfy({ !$0.gameId.isEmpty && !$0.title.isEmpty && $0.sizeBytes >= 0 }) else {
            throw LauncherError("Orbit returned an empty or invalid catalogue.")
        }
        return catalogue
    }

    public var games: [CatalogueGame] {
        Dictionary(grouping: releases, by: \.gameId).map { id, releases in
            CatalogueGame(id: id, releases: releases.sorted { $0.id < $1.id })
        }.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }
}

public struct CatalogueSnapshot: Sendable {
    public let catalogue: OrbitCatalogue
    public let fetchedAt: Date
    public let isCached: Bool
    public let warning: String?
}

public struct LauncherError: LocalizedError, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}
