import Foundation

/// A local, launchable library entry enriched only by an unambiguous catalogue match.
public struct ConsoleGame: Identifiable, Sendable {
    public let local: LocalGame
    public let catalogue: CatalogueGame?
    public var id: String { local.id }
    public var title: String { local.title }
    public var genre: String? { catalogue?.primary.genre }
    public var coverURL: URL? { catalogue?.primary.coverURL }
    public var heroURL: URL? {
        guard let value = catalogue?.primary.hero, let url = URL(string: value),
              url.scheme == "https", url.host != nil else { return nil }
        return url
    }
}

public enum ConsoleLibrary {
    /// Retains the local library's order and entries even when the catalogue is absent or refreshed.
    public static func games(localGames: [LocalGame], catalogueGames: [CatalogueGame]) -> [ConsoleGame] {
        localGames.map { local in
            ConsoleGame(local: local, catalogue: match(local, in: catalogueGames))
        }
    }

    public static func selectedID(_ requestedID: String?, in games: [ConsoleGame]) -> String? {
        if let requestedID, games.contains(where: { $0.id == requestedID }) { return requestedID }
        return games.first?.id
    }

    public static func nextID(after id: String?, in games: [ConsoleGame]) -> String? {
        guard !games.isEmpty else { return nil }
        guard let index = games.firstIndex(where: { $0.id == id }) else { return games.first?.id }
        return games[(index + 1) % games.count].id
    }

    public static func previousID(before id: String?, in games: [ConsoleGame]) -> String? {
        guard !games.isEmpty else { return nil }
        guard let index = games.firstIndex(where: { $0.id == id }) else { return games.last?.id }
        return games[(index + games.count - 1) % games.count].id
    }

    private static func match(_ local: LocalGame, in catalogue: [CatalogueGame]) -> CatalogueGame? {
        let exact = catalogue.filter { $0.id == local.id }
        if !exact.isEmpty { return exact.count == 1 ? exact[0] : nil }

        let pattern = #"(?i)(?:^|[^a-z0-9])(PPSA[0-9]{5})(?=$|[^a-z0-9])"#
        if let expression = try? NSRegularExpression(pattern: pattern) {
            let string = local.id as NSString
            let matches = expression.matches(in: local.id, range: NSRange(location: 0, length: string.length))
            let titleIDs = Set(matches.map { string.substring(with: $0.range(at: 1)).uppercased() })
            if titleIDs.count > 1 { return nil }
            if let titleID = titleIDs.first {
                let candidates = catalogue.filter {
                    $0.releases.contains { $0.titleId.uppercased() == titleID }
                }
                if !candidates.isEmpty { return candidates.count == 1 ? candidates[0] : nil }
            }
        }

        let title = normalizedTitle(local.title)
        guard !title.isEmpty else { return nil }
        let candidates = catalogue.filter { normalizedTitle($0.title) == title }
        return candidates.count == 1 ? candidates[0] : nil
    }

    private static func normalizedTitle(_ title: String) -> String {
        title.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
            .lowercased(with: Locale(identifier: "en_US_POSIX"))
    }
}
