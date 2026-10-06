import Foundation
import LauncherCore
import XCTest

final class ConsoleLibraryTests: XCTestCase {
    private func local(_ id: String, _ title: String) -> LocalGame {
        LocalGame(id: id, title: title, executablePath: "/games/\(id)/eboot.bin", workingDirectory: "/games/\(id)")
    }

    private func catalogue() throws -> [CatalogueGame] {
        try OrbitCatalogue.decode(Data("""
        {"schemaVersion":1,"revision":1,"releases":[
          {"id":"exact","gameId":"attached","titleId":"PPSA00001","title":"Catalog title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"exact.exfat","url":"https://example.com/exact","cover":"https://example.com/exact.png","hero":"https://example.com/hero.png","genre":"Strategy"},
          {"id":"variant","gameId":"attached","titleId":"PPSA00002","title":"Catalog title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"variant.exfat","url":"https://example.com/variant"},
          {"id":"unique","gameId":"unique","titleId":"PPSA00003","title":"Unique Title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"unique.exfat","url":"https://example.com/unique"},
          {"id":"duplicate-a","gameId":"duplicate-a","titleId":"PPSA00004","title":"Same Title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"a.exfat","url":"https://example.com/a"},
          {"id":"duplicate-b","gameId":"duplicate-b","titleId":"PPSA00005","title":"Same Title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"b.exfat","url":"https://example.com/b"},
          {"id":"collision-a","gameId":"collision-a","titleId":"PPSA00006","title":"Unique collision title","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"c.exfat","url":"https://example.com/c"},
          {"id":"collision-b","gameId":"collision-b","titleId":"PPSA00006","title":"Another collision","sizeBytes":1,"provider":"Orbit","format":"exFAT","filename":"d.exfat","url":"https://example.com/d"}
        ]}
        """.utf8)).games
    }

    // Contract: only local entries become console games; their launch paths/title survive enrichment.
    // Regression: a catalogue-driven view hides local-only games, sorts the saved order, or replaces launch metadata.
    // Existing catalogue grouping/persistence tests do not cover the presentation join; no test-only production seam.
    func testLocalEntriesSurviveOfflineAndCatalogueEnrichment() throws {
        let locals = [local("offline", "My local game"), local("attached", "My custom title")]
        let offline = ConsoleLibrary.games(localGames: locals, catalogueGames: [])
        XCTAssertEqual(offline.map(\.id), ["offline", "attached"])
        XCTAssertTrue(offline.allSatisfy { $0.catalogue == nil })
        let enriched = ConsoleLibrary.games(localGames: locals, catalogueGames: try catalogue())
        XCTAssertEqual(enriched.map(\.id), ["offline", "attached"])
        XCTAssertEqual(enriched[1].local, locals[1])
        XCTAssertEqual(enriched[1].title, "My custom title")
        XCTAssertEqual(enriched[1].coverURL?.absoluteString, "https://example.com/exact.png")
        XCTAssertEqual(enriched[1].heroURL?.absoluteString, "https://example.com/hero.png")
        XCTAssertEqual(enriched[1].genre, "Strategy")
        XCTAssertNil(enriched[0].coverURL)
    }

    // Contract: artwork requires a unique identity match, with exact game identity preferred over titles.
    // Regression: first matching title/region wins ambiguously, or fuzzy/prefix title matching assigns another game.
    // Existing catalogue decoding tests cannot catch wrong local-to-catalogue identity joins.
    func testIdentityPrecedenceAndAmbiguityDoNotAssignWrongArtwork() throws {
        let cases: [(String, String, String?)] = [
            ("attached", "Unique Title", "attached"),
            ("import-PPSA00002-copy", "Renamed locally", "attached"),
            ("ppsa00003", "Renamed locally", "unique"),
            ("local-1", "  UNIQUE   Title\n", "unique"),
            ("local-2", "Same Title", nil),
            ("import-PPSA00006", "Unique collision title", nil),
            ("PPSA00001-PPSA00003", "Unique Title", nil),
            ("local-3", "Unique", nil),
            ("local-4", "   ", nil)
        ]
        let games = try catalogue()
        for (id, title, expected) in cases {
            let result = ConsoleLibrary.games(localGames: [local(id, title)], catalogueGames: games)
            XCTAssertEqual(result.first?.catalogue?.id, expected, id)
        }
    }

    // Contract: selection uses saved local identity, unaffected by catalogue order/availability, then falls back locally.
    // Regression: refresh changes the selected index or leaves a deleted selection without a visible focused game.
    // The persistence owner does not exercise transient console selection across catalogue refreshes.
    func testSelectionSurvivesRefreshAndFallsBackAfterRemoval() throws {
        let locals = [local("offline", "Zebra"), local("attached", "Alpha")]
        let catalog = try catalogue()
        for catalogue in [catalog, Array(catalog.reversed()), []] {
            let games = ConsoleLibrary.games(localGames: locals, catalogueGames: catalogue)
            XCTAssertEqual(ConsoleLibrary.selectedID("attached", in: games), "attached")
            XCTAssertEqual(ConsoleLibrary.selectedID("removed", in: games), "offline")
            XCTAssertEqual(ConsoleLibrary.selectedID(nil, in: games), "offline")
        }
        let remaining = ConsoleLibrary.games(localGames: [locals[0]], catalogueGames: catalog)
        XCTAssertEqual(ConsoleLibrary.selectedID("attached", in: remaining), "offline")
        XCTAssertNil(ConsoleLibrary.selectedID("attached", in: []))
    }

    // Contract: directional navigation wraps a local row and handles empty/single rows and absent selection.
    // Regression: end indexing crashes, an empty row invents selection, or keyboard direction skips the boundary item.
    // No existing tests cover console navigation; assertions use explicit visible IDs rather than computed expectations.
    func testDirectionalNavigationAtLibraryBoundaries() {
        let empty: [ConsoleGame] = []
        XCTAssertNil(ConsoleLibrary.nextID(after: nil, in: empty))
        XCTAssertNil(ConsoleLibrary.previousID(before: "removed", in: empty))
        let single = ConsoleLibrary.games(localGames: [local("only", "Only")], catalogueGames: [])
        XCTAssertEqual(ConsoleLibrary.nextID(after: "only", in: single), "only")
        XCTAssertEqual(ConsoleLibrary.previousID(before: "only", in: single), "only")
        let row = ConsoleLibrary.games(localGames: [local("a", "A"), local("b", "B"), local("c", "C")], catalogueGames: [])
        let cases: [(String?, String, String)] = [(nil, "a", "c"), ("removed", "a", "c"), ("a", "b", "c"), ("b", "c", "a"), ("c", "a", "b")]
        for (current, next, previous) in cases {
            XCTAssertEqual(ConsoleLibrary.nextID(after: current, in: row), next)
            XCTAssertEqual(ConsoleLibrary.previousID(before: current, in: row), previous)
        }
    }
}
