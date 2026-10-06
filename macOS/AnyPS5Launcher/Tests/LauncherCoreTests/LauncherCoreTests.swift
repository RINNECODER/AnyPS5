import Foundation
import LauncherCore
import XCTest

final class LauncherCoreTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try FileManager.default.removeItem(at: url) }
        return url
    }

    private let catalogue = Data("""
    {"schemaVersion":1,"revision":13,"releases":[
      {"id":"a-exfat","gameId":"a","titleId":"PPSA00001","title":"Example Game","sizeBytes":42,"provider":"Vikingfile","format":"exFAT","filename":"a.exfat","url":"https://example.com/a","genre":null},
      {"id":"a-ffpfsc","gameId":"a","titleId":"PPSA00002","title":"Example Game","sizeBytes":84,"provider":"Archive.org","format":"FFPFSC","filename":"a.ffpfsc","url":"https://example.com/b"}
    ]}
    """.utf8)

    // Contract: one catalogue game retains all source/format variants and tolerates absent metadata.
    // Regression: release rows become duplicate games or null genre prevents decoding. No launcher coverage exists.
    func testCatalogueGroupsVariantsAndRejectsUnknownSchema() throws {
        let decoded = try OrbitCatalogue.decode(catalogue)
        XCTAssertEqual(decoded.games.count, 1)
        XCTAssertEqual(decoded.games[0].titleIDs, "PPSA00001, PPSA00002")
        XCTAssertEqual(Set(decoded.games[0].releases.map(\.format)), ["exFAT", "FFPFSC"])
        XCTAssertNil(decoded.games[0].primary.genre)
        let future = Data(String(decoding: catalogue, as: UTF8.self).replacingOccurrences(of: "schemaVersion\":1", with: "schemaVersion\":2").utf8)
        XCTAssertThrowsError(try OrbitCatalogue.decode(future))
    }

    // Contract: a failed refresh retains the last valid catalogue and marks it as cached.
    // Regression: HTML/error responses overwrite good offline data. CPU/Metal tests do not use this HTTP boundary.
    func testRefreshPreservesCacheWhenServerReturnsError() async throws {
        let cache = try directory().appendingPathComponent("cache.json")
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [CatalogueProtocol.self]
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }
        let client = CatalogueClient(cacheURL: cache, session: session)
        CatalogueProtocol.set(response: catalogue, status: 200)
        let first = try await client.refresh()
        XCTAssertFalse(first.isCached)
        CatalogueProtocol.set(response: Data("<html>unavailable</html>".utf8), status: 503)
        let second = try await client.refresh()
        XCTAssertTrue(second.isCached)
        XCTAssertNotNil(second.warning)
        XCTAssertEqual(second.catalogue.games[0].title, "Example Game")
        XCTAssertEqual(try Data(contentsOf: cache), catalogue)
    }

    // Contract: catalogue associations, engine path and replacement resource directories survive restart.
    // Regression: attaching a replacement duplicates a game or drops its paths; no prior persistence tests exist.
    func testLibraryReplacementRoundTrips() throws {
        let storage = LibraryPersistence(url: try directory().appendingPathComponent("library.json"))
        var library = try storage.load()
        library.enginePath = "/engine with spaces/anyps5_cpu_run"
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/old.elf", workingDirectory: "/old"))
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/new.elf", workingDirectory: "/resources"))
        try storage.save(library)
        let restored = try storage.load()
        XCTAssertEqual(restored.games.count, 1)
        XCTAssertEqual(restored.games[0].executablePath, "/new.elf")
        XCTAssertEqual(restored.games[0].workingDirectory, "/resources")
        XCTAssertEqual(restored.enginePath, "/engine with spaces/anyps5_cpu_run")
    }

    private func guest(in folder: URL) throws -> LocalGame {
        var header = Data(repeating: 0, count: 64)
        header.replaceSubrange(0..<7, with: [0x7f, 0x45, 0x4c, 0x46, 2, 1, 1])
        header[16] = 2
        header[18] = 62
        let input = folder.appendingPathComponent("guest $(no-shell) name.elf")
        try header.write(to: input)
        return LocalGame(id: "local", title: "Fixture", executablePath: input.path, workingDirectory: folder.path)
    }

    // Contract: launch passes the guest path literally, preserves cwd, captures both streams and actual exit status.
    // Regression: shell interpolation, lost stderr or false success. This tests the new real subprocess boundary.
    func testEngineArgumentsOutputAndFailureExit() async throws {
        let folder = try directory()
        let game = try guest(in: folder)
        let engine = folder.appendingPathComponent("engine")
        try Data("#!/bin/sh\nprintf 'guest=%s\\ncwd=%s\\n' \"$1\" \"$PWD\"\nfor attempt in 1 2 3 4 5 6 7 8 9 10; do\n[ -f observed ] && break\nsleep 0.1\ndone\n[ -f observed ] || exit 8\nprintf 'unsupported import\\n' >&2\nexit 7\n".utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let runner = EngineRunner()
        var output = Data()
        var status: Int32?
        for try await event in try runner.run(engine: engine, game: game) {
            switch event {
            case .output(let bytes):
                output.append(bytes)
                // The child waits for this acknowledgement: output must stream before it exits.
                try Data().write(to: folder.appendingPathComponent("observed"))
            case .exited(let code): status = code
            }
        }
        let text = String(decoding: output, as: UTF8.self)
        XCTAssertTrue(text.contains("guest=\(game.executablePath)\n"))
        let cwd = try XCTUnwrap(text.split(separator: "\n").first { $0.hasPrefix("cwd=") }).dropFirst(4)
        let actualDirectory = try FileManager.default.attributesOfItem(atPath: String(cwd))
        let expectedDirectory = try FileManager.default.attributesOfItem(atPath: folder.path)
        XCTAssertEqual(actualDirectory[.systemFileNumber] as? NSNumber, expectedDirectory[.systemFileNumber] as? NSNumber)
        XCTAssertEqual(actualDirectory[.systemNumber] as? NSNumber, expectedDirectory[.systemNumber] as? NSNumber)
        XCTAssertTrue(text.contains("unsupported import\n"))
        XCTAssertEqual(status, 7)
    }

    // Contract: console containers cannot start an engine; static CPU mode refuses PS5 OSABI inputs explicitly.
    // Regression: treating a downloaded container as a runnable ELF. The subprocess test only covers valid headers.
    func testEngineRejectsContainersAndWrongABI() throws {
        let folder = try directory()
        let engine = folder.appendingPathComponent("anyps5_cpu_run")
        try Data("#!/bin/sh\nexit 0\n".utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let game = try guest(in: folder)
        let input = URL(fileURLWithPath: game.executablePath)
        try Data(repeating: 0, count: 64).write(to: input)
        XCTAssertThrowsError(try EngineRunner.validate(engine: engine, game: game)) { error in
            XCTAssertTrue(error.localizedDescription.contains("clean x86-64 ELF"))
        }
        _ = try guest(in: folder)
        var bytes = try Data(contentsOf: input)
        bytes[7] = 9
        try bytes.write(to: input)
        XCTAssertThrowsError(try EngineRunner.validate(engine: engine, game: game)) { error in
            XCTAssertTrue(error.localizedDescription.contains("PS5 game loader"))
        }
    }

    // Contract: capability probing reads schema1 native architecture and format support from the executable.
    // Regression: assuming readiness by filename or accepting an incompatible schema. Static process tests lack this protocol.
    func testCapabilityProbeAndSchemaGate() async throws {
        let payload = """
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["static_elf64_x86_64"],"runtime_abi":"linux_sysv","ps5_game_runtime_ready":false}
        """
        let folder = try directory()
        let engine = folder.appendingPathComponent("renamed-engine")
        try Data("#!/bin/sh\n[ \"$1\" = '--capabilities-json' ] || exit 9\ncat <<'JSON'\n\(payload)\nJSON\n".utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let result = try await EngineCapabilities.probe(engine)
        XCTAssertFalse(result.ps5GameRuntimeReady)
        XCTAssertEqual(result.supportedFormats, ["static_elf64_x86_64"])
        XCTAssertEqual(result.backend, "unicorn")
        XCTAssertThrowsError(try EngineCapabilities.decode(Data(payload.replacingOccurrences(of: "schema_version\":1", with: "schema_version\":2").utf8)))
    }

    // Contract: the launcher invokes the actual ARM64 AnyPS5 engine and preserves independently expected guest results.
    // Regression: capability flags or process plumbing break the real CLI while a shell fixture still succeeds.
    func testActualAnyPS5Checkpoint() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard let path = environment["ANYPS5_ENGINE"], let guest = environment["ANYPS5_GUEST_FIXTURE"] else {
            throw XCTSkip("Set ANYPS5_ENGINE and ANYPS5_GUEST_FIXTURE to run the real runtime integration check.")
        }
        let engine = URL(fileURLWithPath: path)
        let capabilities = try? await EngineCapabilities.probe(engine)
        let game = LocalGame(id: "homebrew", title: "Homebrew", executablePath: guest,
                             workingDirectory: URL(fileURLWithPath: guest).deletingLastPathComponent().path)
        let runner = EngineRunner()
        var output = Data()
        var exit: Int32?
        for try await event in try runner.run(engine: engine, game: game, capabilities: capabilities) {
            switch event { case .output(let bytes): output.append(bytes); case .exited(let code): exit = code }
        }
        XCTAssertEqual(exit, 0)
        // Prime count and sum for <=1000; CRC of the fixture's independently specified 4096-byte pattern.
        XCTAssertTrue(String(decoding: output, as: UTF8.self).contains("homebrew primes=168 sum=76127 buffer_crc32=2511520486 tls=ok bss=ok\n"), String(decoding: output, as: UTF8.self))
    }
}

private final class CatalogueProtocol: URLProtocol {
    private static let lock = NSLock()
    private static var response = Data()
    private static var status = 200
    static func set(response: Data, status: Int) {
        lock.lock(); defer { lock.unlock() }
        self.response = response; self.status = status
    }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.lock.lock()
        let data = Self.response, status = Self.status
        Self.lock.unlock()
        client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: nil, headerFields: nil)!, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
