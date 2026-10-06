import Foundation
import CryptoKit
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

    // Contract: associations retain the original image across restarts, and older folder-based libraries still load.
    // Regression: replacement drops the image path or the new field makes existing libraries undecodable.
    // This is the persistence owner; subprocess and image tests do not exercise saved library migrations.
    func testLibraryReplacementRoundTrips() throws {
        let url = try directory().appendingPathComponent("library.json")
        let storage = LibraryPersistence(url: url)
        var library = try storage.load()
        library.enginePath = "/engine with spaces/anyps5_cpu_run"
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/old.elf", workingDirectory: "/old"))
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/new.elf", workingDirectory: "/resources",
                                 resourceImagePath: "/downloads/game image.exfat"))
        try storage.save(library)
        let restored = try storage.load()
        XCTAssertEqual(restored.games.count, 1)
        XCTAssertEqual(restored.games[0].executablePath, "/new.elf")
        XCTAssertEqual(restored.games[0].workingDirectory, "/resources")
        XCTAssertEqual(restored.games[0].resourceImagePath, "/downloads/game image.exfat")
        XCTAssertEqual(restored.enginePath, "/engine with spaces/anyps5_cpu_run")
        try Data("""
        {"enginePath":"/legacy-engine","games":[{"id":"legacy","title":"Existing game","executablePath":"/existing.elf","workingDirectory":"/existing-resources"}]}
        """.utf8).write(to: url)
        let legacy = try storage.load()
        XCTAssertEqual(legacy.games.count, 1)
        XCTAssertEqual(legacy.games[0].workingDirectory, "/existing-resources")
        XCTAssertNil(legacy.games[0].resourceImagePath)
        XCTAssertEqual(legacy.enginePath, "/legacy-engine")
    }

    // Contract: incomplete downloads and inconsistent raw-volume sizes are rejected before an OS mount.
    // Regression: a renamed partial download or overflowing sector count passes preflight.
    // Existing ELF guards only inspect executables; this exercises the public image boundary without a mock.
    func testExFATPreflightRejectsIncompleteAndMalformedImages() throws {
        let folder = try directory()
        var complete = Data(repeating: 0, count: 4096)
        complete.replaceSubrange(3..<11, with: Data("EXFAT   ".utf8))
        complete[72] = 8 // Eight 512-byte sectors, independently matching this fixture's length.
        complete[108] = 9
        complete[510] = 0x55
        complete[511] = 0xaa
        let valid = folder.appendingPathComponent("complete.exfat")
        try complete.write(to: valid)
        try ExFATResources.validate(valid)

        var overflowing = complete
        overflowing.replaceSubrange(72..<80, with: Data(repeating: 0xff, count: 8))
        var badSignature = complete
        badSignature[511] = 0
        let cases: [(String, Data, String)] = [
            ("complete.crdownload", complete, "still downloading"),
            ("renamed-partial.exfat", Data(complete.prefix(512)), "size does not match"),
            ("overflow.exfat", overflowing, "size does not match"),
            ("bad-signature.exfat", badSignature, "not a raw exFAT volume"),
            ("short-header.exfat", Data(complete.prefix(128)), "not a raw exFAT volume")
        ]
        for (name, bytes, diagnostic) in cases {
            let input = folder.appendingPathComponent(name)
            try bytes.write(to: input)
            XCTAssertThrowsError(try ExFATResources.validate(input), name) { error in
                XCTAssertTrue(error.localizedDescription.contains(diagnostic), "\(name): \(error)")
            }
        }
    }

    // Contract: sessions are read-only, reject known attachments with an actionable preflight diagnostic, and preserve bytes.
    // Regression: duplicate attachment falls through to an unrelated OS error instead of explaining the existing volume.
    // Header validation cannot establish OS mount permissions or lifecycle; the fixture uses the actual public API.
    func testActualReadOnlyExFATResourceSession() async throws {
        guard let path = ProcessInfo.processInfo.environment["ANYPS5_EXFAT_FIXTURE"] else {
            throw XCTSkip("Set ANYPS5_EXFAT_FIXTURE to a system-created image containing fixture-resource.txt.")
        }
        let image = URL(fileURLWithPath: path)
        let size = try FileManager.default.attributesOfItem(atPath: path)[.size] as? NSNumber
        guard let size, size.uint64Value <= 64 * 1024 * 1024 else {
            throw XCTSkip("The OS mount check requires a small non-game fixture, at most 64 MiB.")
        }
        let originalDigest = SHA256.hash(data: try Data(contentsOf: image))
        let session = try await ExFATResources.mount(image)
        let resource = session.directory.appendingPathComponent("fixture-resource.txt")
        do {
            XCTAssertEqual(try String(contentsOf: resource, encoding: .utf8), "AnyPS5 read-only resource fixture\n")
            do {
                let duplicate = try await ExFATResources.mount(image)
                XCTFail("An already attached source image must be rejected without taking ownership of its mount.")
                try await duplicate.unmount()
            } catch {
                XCTAssertTrue(error.localizedDescription.hasPrefix("This resource image is already attached."),
                              "Duplicate attachment must fail at public preflight, not an unrelated OS error: \(error)")
            }
            XCTAssertEqual(try String(contentsOf: resource, encoding: .utf8), "AnyPS5 read-only resource fixture\n",
                           "Rejecting duplicate attachment must preserve access to the existing mounted volume.")
            XCTAssertThrowsError(try Data("must not change the resource\n".utf8).write(to: resource))
            XCTAssertThrowsError(try Data().write(to: session.directory.appendingPathComponent("must-not-create.txt")))
            try await session.unmount()
        } catch {
            try? await session.unmount()
            throw error
        }
        XCTAssertThrowsError(try Data(contentsOf: resource), "Unmount must release access to the mounted resource.")
        XCTAssertEqual(SHA256.hash(data: try Data(contentsOf: image)), originalDigest,
                       "Reading resources and unmounting must preserve the original image byte for byte.")
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

    // Contract: resource argv and writable working directory remain separate, with legacy cwd fallback and live output.
    // Regression: an advertised root still changes cwd to the mounted image, or the override is lost/interpolated.
    // This subprocess boundary owns routing; prior same-directory cases and the CPU oracle cannot prove separation.
    func testEngineArgumentsOutputAndFailureExit() async throws {
        let folder = try directory().appendingPathComponent("resources $(no-shell) folder", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        let separateResources = folder.appendingPathComponent("mounted $(literal) resource folder", isDirectory: true)
        try FileManager.default.createDirectory(at: separateResources, withIntermediateDirectories: false)
        let game = try guest(in: folder)
        let engine = folder.appendingPathComponent("engine")
        try Data("#!/bin/sh\nif [ \"$1\" = '--resource-root' ]; then\nprintf 'root=%s\\n' \"$2\"\nshift 2\nfi\nif [ \"$1\" = '--diagnostics-json' ]; then printf 'diagnostics\\n'; shift; fi\n[ \"$#\" = 1 ] || exit 9\nprintf 'guest=%s\\ncwd=%s\\n' \"$1\" \"$PWD\"\nfor attempt in 1 2 3 4 5 6 7 8 9 10; do\n[ -f observed ] && break\nsleep 0.1\ndone\n[ -f observed ] || exit 8\nprintf 'unsupported import\\n' >&2\nexit 7\n".utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let payload = """
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["static_elf64_x86_64"],"resource_root_argument":"--resource-root","runtime_abi":"linux_sysv","ps5_game_runtime_ready":false}
        """
        let capabilities = try EngineCapabilities.decode(Data(payload.utf8))
        let legacyCapabilities = try EngineCapabilities.decode(Data(payload.replacingOccurrences(of:
            "\"resource_root_argument\":\"--resource-root\",", with: "").utf8))
        for override: URL? in [nil, separateResources] {
            let resourceRoot = override ?? folder
            for contract: EngineCapabilities? in [nil, legacyCapabilities, capabilities] {
                let hasResourceArgument = contract?.resourceRootArgument == "--resource-root"
                let expectedCWD = hasResourceArgument ? folder : resourceRoot
                // Remove every previous marker, so choosing the wrong cwd cannot reuse an acknowledgement.
                for location in [folder, separateResources] {
                    let oldMarker = location.appendingPathComponent("observed")
                    if FileManager.default.fileExists(atPath: oldMarker.path) { try FileManager.default.removeItem(at: oldMarker) }
                }
                let marker = expectedCWD.appendingPathComponent("observed")
                let runner = EngineRunner()
                var output = Data()
                var status: Int32?
                for try await event in try runner.run(engine: engine, game: game, capabilities: contract, resourceDirectory: override) {
                    switch event {
                    case .output(let bytes):
                        output.append(bytes)
                        // The child waits for this acknowledgement: output must stream before it exits.
                        try Data().write(to: marker)
                    case .exited(let code): status = code
                    }
                }
                let text = String(decoding: output, as: UTF8.self)
                XCTAssertTrue(text.contains("guest=\(game.executablePath)\n"))
                XCTAssertEqual(text.contains("diagnostics\n"), contract != nil)
                if hasResourceArgument { XCTAssertTrue(text.contains("root=\(resourceRoot.path)\n"), text) }
                else { XCTAssertFalse(text.contains("root="), text) }
                let cwd = try XCTUnwrap(text.split(separator: "\n").first { $0.hasPrefix("cwd=") }).dropFirst(4)
                let actualDirectory = try FileManager.default.attributesOfItem(atPath: String(cwd))
                let expectedDirectory = try FileManager.default.attributesOfItem(atPath: expectedCWD.path)
                XCTAssertEqual(actualDirectory[.systemFileNumber] as? NSNumber, expectedDirectory[.systemFileNumber] as? NSNumber)
                XCTAssertEqual(actualDirectory[.systemNumber] as? NSNumber, expectedDirectory[.systemNumber] as? NSNumber)
                XCTAssertTrue(text.contains("unsupported import\n"))
                XCTAssertEqual(status, 7)
            }
        }
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
            XCTAssertTrue(error.localizedDescription.contains("supported x86-64 ELF"))
        }
        _ = try guest(in: folder)
        var bytes = try Data(contentsOf: input)
        bytes[7] = 9
        try bytes.write(to: input)
        XCTAssertThrowsError(try EngineRunner.validate(engine: engine, game: game)) { error in
            XCTAssertTrue(error.localizedDescription.contains("PS5 game loader"))
        }
        // SELF routing must require an advertised capability; the engine owns per-segment validation.
        // Regression: the legacy runner accepts wrapped inputs, or the launcher blocks a capable engine.
        bytes.replaceSubrange(0..<4, with: [0x4f, 0x15, 0x3d, 0x1d])
        try bytes.write(to: input)
        XCTAssertThrowsError(try EngineRunner.validate(engine: engine, game: game)) { error in
            XCTAssertTrue(error.localizedDescription.contains("does not advertise plaintext SELF"))
        }
        let capabilities = try EngineCapabilities.decode(Data("""
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["sce_elf64_x86_64"],"supported_containers":["plain_self"],"runtime_abi":"sce_sysv","ps5_game_runtime_ready":false}
        """.utf8))
        try EngineRunner.validate(engine: engine, game: game, capabilities: capabilities)
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

    // Contract: inspection never launches a guest, retains restrictions, and refuses rejected/unknown reports.
    // Regression: using the run flag or swallowing engine exit126 presents a false ready state.
    // Existing subprocess coverage launches guests and cannot protect the non-executing inspection protocol.
    func testInspectionProtocolAndRejection() async throws {
        let folder = try directory()
        let game = try guest(in: folder)
        let engine = folder.appendingPathComponent("inspector")
        let capabilities = try EngineCapabilities.decode(Data("""
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["sce_elf64_x86_64"],"runtime_abi":"sce_sysv","ps5_game_runtime_ready":false}
        """.utf8))
        let report = """
        {"schema_version":1,"event":"inspection","format":"sce_elf64_x86_64","segment_count":2,"relocation_count":1,"has_tls":true,"has_process_parameters":false,"imports":[{"nid":"required-NID","library":"libkernel","module":"libkernel"}],"needed_modules":["libkernel"],"needed_files":[],"unsupported_reasons":["guest TLS unsupported"]}
        """
        // No shell is used by production; the test child checks its literal argv independently.
        let literalPath = "'" + game.executablePath.replacingOccurrences(of: "'", with: "'\\''") + "'"
        let script = "#!/bin/sh\n[ \"$1\" = '--inspect-sce-json' ] || exit 91\n[ \"$2\" = \(literalPath) ] || exit 92\ncat <<'JSON'\n\(report)\nJSON\n"
        try Data(script.utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let result = try await EngineInspection.inspect(engine: engine, game: game, capabilities: capabilities)
        XCTAssertEqual(result.neededModules, ["libkernel"])
        XCTAssertTrue(result.hasTLS)
        XCTAssertTrue(result.summary.contains("guest TLS unsupported"))
        XCTAssertTrue(result.summary.contains("required-NID"))
        for rejection in ["printf 'missing segment\\n' >&2\nexit 126\n",
                          "cat <<'JSON'\n\(report.replacingOccurrences(of: "schema_version\":1", with: "schema_version\":2"))\nJSON\n"] {
            try Data(("#!/bin/sh\n" + rejection).utf8).write(to: engine)
            do {
                _ = try await EngineInspection.inspect(engine: engine, game: game, capabilities: capabilities)
                XCTFail("A rejected or unknown report must not produce an inspection result.")
            } catch {
                XCTAssertTrue(error.localizedDescription.contains(rejection.contains("exit 126") ? "missing segment" : "unsupported inspection protocol"))
            }
        }
    }

    // Contract: the actual engine preserves static guest results and enabled SCE inspection's qualified imports through display.
    // Regression: a failed probe silently skips SCE coverage, or an older CLI/decoder/summary loses service families.
    // Shell protocol tests cannot establish the real engine report; CPU tests do not own launcher decoding or display.
    func testActualAnyPS5Checkpoint() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard let path = environment["ANYPS5_ENGINE"], let guest = environment["ANYPS5_GUEST_FIXTURE"] else {
            throw XCTSkip("Set ANYPS5_ENGINE and ANYPS5_GUEST_FIXTURE to run the real runtime integration check.")
        }
        let engine = URL(fileURLWithPath: path)
        let capabilities = try await EngineCapabilities.probe(engine)
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
        if let sceGuest = environment["ANYPS5_SCE_GUEST_FIXTURE"] {
            let sceGame = LocalGame(id: "sce-homebrew", title: "SCE Homebrew", executablePath: sceGuest,
                                    workingDirectory: URL(fileURLWithPath: sceGuest).deletingLastPathComponent().path)
            let inspection = try await EngineInspection.inspect(engine: engine, game: sceGame, capabilities: capabilities)
            XCTAssertEqual(inspection.format, "sce_elf64_x86_64")
            XCTAssertEqual(Set(inspection.neededModules), ["libc", "libkernel", "libSceUserService", "libSceSystemService"])
            XCTAssertEqual(inspection.imports.count, 18)
            let representatives: [(nid: String, library: String, module: String)] = [
                ("Q3VBxCXhUHs", "libc", "libc"),
                ("1G3lF1Gg1k8", "libkernel", "libkernel"),
                ("CdWp0oHWGr0", "libSceUserService", "libSceUserService"),
                ("fZo48un7LK4", "libSceSystemService", "libSceSystemService")
            ]
            let summaryLines = inspection.summary.components(separatedBy: "\n")
            XCTAssertTrue(summaryLines.contains("Imports (18):"))
            for expected in representatives {
                XCTAssertTrue(inspection.imports.contains {
                    $0.nid == expected.nid && $0.library == expected.library && $0.module == expected.module
                }, "Inspection lost qualified import \(expected.nid)")
                XCTAssertTrue(summaryLines.contains("  \(expected.module) / \(expected.library) / \(expected.nid)"),
                              "Displayed inspection lost qualified import \(expected.nid)")
            }
            XCTAssertFalse(inspection.hasTLS)
            XCTAssertTrue(inspection.unsupportedReasons.isEmpty)
        }
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
