import Foundation
import LauncherCore
import XCTest

final class MacPSStorageTests: XCTestCase {
    // Contract: the app rename preserves saved library bytes and all external engine/game paths,
    // never overwrites an existing MacPS library, and refuses malformed legacy data.
    // Regression: switching only the directory silently creates an empty library, or every launch
    // replaces edits with the old library. Persistence round-trip tests do not cover directory migration.
    // Uses real temporary files at the production storage boundary; no test-only seam.
    func testRenamePreservesLibraryAndNeverOverwritesCurrentState() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let legacy = root.appendingPathComponent("AnyPS5Launcher")
        try FileManager.default.createDirectory(at: legacy, withIntermediateDirectories: true)
        let original = Data("""
        {"games":[{"id":"saved","title":"Saved game","executablePath":"/original/games/eboot.bin",
        "workingDirectory":"/original/resources","resourceImagePath":"/original/game.exfat",
        "sceModulePaths":["/original/libc.prx"]}],"enginePath":"/original/engine/anyps5_cpu_run",
        "enginePackageManifestSHA256":"unchanged-hash"}
        """.utf8)
        let oldFile = legacy.appendingPathComponent("library.json")
        try original.write(to: oldFile)
        let directory = try MacPSStorage.prepare(in: root)
        let storage = LibraryPersistence(url: directory.appendingPathComponent("library.json"))
        let migrated = try storage.load()
        XCTAssertEqual(migrated.games.first?.id, "saved")
        XCTAssertEqual(migrated.games.first?.executablePath, "/original/games/eboot.bin")
        XCTAssertEqual(migrated.games.first?.workingDirectory, "/original/resources")
        XCTAssertEqual(migrated.games.first?.resourceImagePath, "/original/game.exfat")
        XCTAssertEqual(migrated.games.first?.sceModulePaths, ["/original/libc.prx"])
        XCTAssertEqual(migrated.enginePath, "/original/engine/anyps5_cpu_run")
        XCTAssertEqual(migrated.enginePackageManifestSHA256, "unchanged-hash")
        XCTAssertEqual(try Data(contentsOf: directory.appendingPathComponent("library.json")), original)
        XCTAssertEqual(try Data(contentsOf: oldFile), original)

        var current = migrated
        current.enginePath = "/new/selection"
        try storage.save(current)
        _ = try MacPSStorage.prepare(in: root)
        XCTAssertEqual(try storage.load().enginePath, "/new/selection")

        let invalidRoot = root.appendingPathComponent("invalid")
        let invalidLegacy = invalidRoot.appendingPathComponent("AnyPS5Launcher")
        try FileManager.default.createDirectory(at: invalidLegacy, withIntermediateDirectories: true)
        let invalidFile = invalidLegacy.appendingPathComponent("library.json")
        let invalid = Data("invalid-library".utf8)
        try invalid.write(to: invalidFile)
        XCTAssertThrowsError(try MacPSStorage.prepare(in: invalidRoot))
        XCTAssertEqual(try Data(contentsOf: invalidFile), invalid)
        XCTAssertFalse(FileManager.default.fileExists(atPath: invalidRoot.appendingPathComponent("MacPS/library.json").path))
    }
}
