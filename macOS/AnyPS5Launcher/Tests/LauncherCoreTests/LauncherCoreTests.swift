import Foundation
import CryptoKit
import Darwin
import LauncherCore
import XCTest

final class LauncherCoreTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try FileManager.default.removeItem(at: url) }
        return url
    }

    private func copiedEnginePackage() throws -> URL {
        guard let path = ProcessInfo.processInfo.environment["ANYPS5_ENGINE_PACKAGE"] else {
            throw XCTSkip("Set ANYPS5_ENGINE_PACKAGE to the frozen native module/CRT package.")
        }
        let copy = try directory().appendingPathComponent("relocated engine $(literal) 'é' package")
        try FileManager.default.copyItem(at: URL(fileURLWithPath: path), to: copy)
        // The frozen source is read-only. Only this owned copy becomes writable for controls/cleanup.
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: copy.path)
        let items = try XCTUnwrap(FileManager.default.enumerator(at: copy, includingPropertiesForKeys: [.isDirectoryKey]))
        for case let item as URL in items {
            let attributes = try FileManager.default.attributesOfItem(atPath: item.path)
            let permissions = (attributes[.posixPermissions] as? NSNumber)?.intValue ?? 0o644
            let isDirectory = try item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory == true
            try FileManager.default.setAttributes([.posixPermissions: permissions | (isDirectory ? 0o700 : 0o200)],
                                                 ofItemAtPath: item.path)
        }
        return copy.resolvingSymlinksInPath()
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

    // Contract: saved associations retain image/module paths and accepted package identity; older libraries decode without either.
    // Regression: decoding requires new keys, drops malformed lists or loses identity so a changed engine can regain legacy routing.
    // This is the persistence owner; subprocess and image tests do not exercise saved library migrations.
    func testLibraryReplacementRoundTrips() throws {
        let url = try directory().appendingPathComponent("library.json")
        let storage = LibraryPersistence(url: url)
        var library = try storage.load()
        library.enginePath = "/engine with spaces/anyps5_cpu_run"
        library.enginePackageManifestSHA256 = String(repeating: "a", count: 64)
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/old.elf", workingDirectory: "/old"))
        let modules = ["/libraries/z $(literal) module.prx", "/libraries/a quoted 'module'.prx"]
        library.attach(LocalGame(id: "a", title: "Example", executablePath: "/new.elf", workingDirectory: "/resources",
                                 resourceImagePath: "/downloads/game image.exfat", sceModulePaths: modules))
        try storage.save(library)
        let restored = try storage.load()
        XCTAssertEqual(restored.games.count, 1)
        XCTAssertEqual(restored.games[0].executablePath, "/new.elf")
        XCTAssertEqual(restored.games[0].workingDirectory, "/resources")
        XCTAssertEqual(restored.games[0].resourceImagePath, "/downloads/game image.exfat")
        XCTAssertEqual(restored.games[0].sceModulePaths, modules)
        XCTAssertEqual(restored.enginePath, "/engine with spaces/anyps5_cpu_run")
        XCTAssertEqual(restored.enginePackageManifestSHA256, String(repeating: "a", count: 64))
        try Data("""
        {"enginePath":"/legacy-engine","games":[{"id":"legacy","title":"Existing game","executablePath":"/existing.elf","workingDirectory":"/existing-resources"}]}
        """.utf8).write(to: url)
        let legacy = try storage.load()
        XCTAssertEqual(legacy.games.count, 1)
        XCTAssertEqual(legacy.games[0].workingDirectory, "/existing-resources")
        XCTAssertNil(legacy.games[0].resourceImagePath)
        XCTAssertEqual(legacy.games[0].sceModulePaths, [])
        XCTAssertEqual(legacy.enginePath, "/legacy-engine")
        XCTAssertNil(legacy.enginePackageManifestSHA256)
        try Data("""
        {"games":[{"id":"bad","title":"Malformed","executablePath":"/existing.elf","workingDirectory":"/existing-resources","sceModulePaths":"not-an-array"}],"enginePath":"/engine"}
        """.utf8).write(to: url)
        XCTAssertThrowsError(try storage.load(), "Malformed module associations must not silently become an empty list.")
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

    // Contract: one started event precedes output/exit and identifies the real child and literal invocation; unsupported modules cannot start it.
    // Regression: start evidence is missing, early/fabricated, duplicated, delayed or describes different argv/cwd.
    // This real-child recorder owns the protocol; CPU linking tests cannot protect Swift startup evidence or invocation routing.
    func testEngineArgumentsOutputAndFailureExit() async throws {
        let folder = try directory().appendingPathComponent("resources $(no-shell) folder", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
        let separateResources = folder.appendingPathComponent("mounted $(literal) resource folder", isDirectory: true)
        try FileManager.default.createDirectory(at: separateResources, withIntermediateDirectories: false)
        let game = try guest(in: folder)
        let engine = folder.appendingPathComponent("engine")
        let script = #"""
        #!/bin/sh
        : > started
        printf 'pid=%s\n' "$$"
        printf 'arg=%s\n' "$@"
        while [ "$#" -gt 1 ]; do
          case "$1" in
            --resource-root) printf 'root=%s\n' "$2"; shift 2 ;;
            --diagnostics-json) printf 'diagnostics\n'; shift ;;
            --sce-module) shift 2 ;;
            *) exit 9 ;;
          esac
        done
        [ "$#" = 1 ] || exit 9
        printf 'guest=%s\ncwd=%s\n' "$1" "$PWD"
        for attempt in 1 2 3 4 5 6 7 8 9 10; do
          [ -f observed ] && break
          sleep 0.1
        done
        [ -f observed ] || exit 8
        printf 'unsupported import\n' >&2
        exit 7
        """#
        try Data(script.utf8).write(to: engine)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engine.path)
        let payload = """
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["static_elf64_x86_64"],"resource_root_argument":"--resource-root","runtime_abi":"linux_sysv","ps5_game_runtime_ready":false}
        """
        let capabilities = try EngineCapabilities.decode(Data(payload.utf8))
        let legacyCapabilities = try EngineCapabilities.decode(Data(payload.replacingOccurrences(of:
            "\"resource_root_argument\":\"--resource-root\",", with: "").utf8))
        func clearMarkers() throws {
            for location in [folder, separateResources] {
                for name in ["observed", "started"] {
                    let marker = location.appendingPathComponent(name)
                    if FileManager.default.fileExists(atPath: marker.path) { try FileManager.default.removeItem(at: marker) }
                }
            }
        }
        typealias ProcessStart = (pid: Int32, executable: String, arguments: [String], workingDirectory: String)
        func recordRun(_ target: LocalGame, _ contract: EngineCapabilities?, _ override: URL?) async throws -> (String, Int32?, ProcessStart) {
            try clearMarkers()
            let root = override ?? folder
            let cwd = contract?.resourceRootArgument == "--resource-root" ? folder : root
            let marker = cwd.appendingPathComponent("observed")
            let runner = EngineRunner()
            var output = Data()
            var status: Int32?
            var starts: [ProcessStart] = []
            for try await event in try runner.run(engine: engine, game: target, capabilities: contract, resourceDirectory: override) {
                switch event {
                case .started(let pid, let executable, let arguments, let workingDirectory):
                    XCTAssertTrue(starts.isEmpty, "A child must have exactly one started event.")
                    XCTAssertTrue(output.isEmpty, "Started evidence must precede the first output.")
                    XCTAssertNil(status, "Started evidence must precede exit.")
                    starts.append((pid, executable, arguments, workingDirectory))
                case .output(let bytes):
                    XCTAssertEqual(starts.count, 1, "Output arrived without exactly one prior started event.")
                    output.append(bytes)
                    // The child waits for this acknowledgement: output must stream before it exits.
                    try Data().write(to: marker)
                case .exited(let code):
                    XCTAssertEqual(starts.count, 1, "Exit arrived without exactly one prior started event.")
                    status = code
                }
            }
            XCTAssertEqual(starts.count, 1)
            return (String(decoding: output, as: UTF8.self), status, try XCTUnwrap(starts.first))
        }
        func checkOutput(_ text: String, _ status: Int32?, _ start: ProcessStart, _ target: LocalGame, _ contract: EngineCapabilities?,
                         _ override: URL?, _ expectedModuleArguments: [String] = []) throws {
            let resourceRoot = override ?? folder
            let hasResourceArgument = contract?.resourceRootArgument == "--resource-root"
            let expectedCWD = hasResourceArgument ? folder : resourceRoot
            let expectedArguments = (hasResourceArgument ? ["--resource-root", resourceRoot.path] : []) +
                (contract != nil ? ["--diagnostics-json"] : []) +
                expectedModuleArguments + [target.executablePath]
            let recordedArguments = text.components(separatedBy: "\n").filter { $0.hasPrefix("arg=") }.map { String($0.dropFirst(4)) }
            XCTAssertEqual(recordedArguments, expectedArguments, "Literal argv order/pairing changed: \(text)")
            let pidLine = try XCTUnwrap(text.split(separator: "\n").first { $0.hasPrefix("pid=") })
            let actualPID = try XCTUnwrap(Int32(pidLine.dropFirst(4)))
            XCTAssertGreaterThan(actualPID, 1)
            XCTAssertEqual(start.pid, actualPID, "Started PID must match the child's own shell builtin $$.")
            XCTAssertEqual(start.executable, engine.path)
            XCTAssertEqual(start.arguments, expectedArguments, "Started evidence describes a different literal invocation.")
            XCTAssertTrue(text.contains("guest=\(target.executablePath)\n"))
            XCTAssertEqual(text.contains("diagnostics\n"), contract != nil)
            if hasResourceArgument { XCTAssertTrue(text.contains("root=\(resourceRoot.path)\n"), text) }
            else { XCTAssertFalse(text.contains("root="), text) }
            let cwd = try XCTUnwrap(text.split(separator: "\n").first { $0.hasPrefix("cwd=") }).dropFirst(4)
            let expectedDirectory = try FileManager.default.attributesOfItem(atPath: expectedCWD.path)
            for actualPath in [String(cwd), start.workingDirectory] {
                let actualDirectory = try FileManager.default.attributesOfItem(atPath: actualPath)
                XCTAssertEqual(actualDirectory[.systemFileNumber] as? NSNumber, expectedDirectory[.systemFileNumber] as? NSNumber)
                XCTAssertEqual(actualDirectory[.systemNumber] as? NSNumber, expectedDirectory[.systemNumber] as? NSNumber)
            }
            XCTAssertTrue(text.contains("unsupported import\n"))
            XCTAssertEqual(status, 7)
        }
        for override: URL? in [nil, separateResources] {
            for contract: EngineCapabilities? in [nil, legacyCapabilities, capabilities] {
                let (text, status, start) = try await recordRun(game, contract, override)
                try checkOutput(text, status, start, game, contract, override)
            }
        }
        var sceHeader = try Data(contentsOf: URL(fileURLWithPath: game.executablePath))
        sceHeader[7] = 9; sceHeader[8] = 2; sceHeader[16] = 0x10; sceHeader[17] = 0xfe
        let sceExecutable = folder.appendingPathComponent("SCE guest $(literal) name.elf")
        try sceHeader.write(to: sceExecutable)
        let modules = ["z $(literal) ; module.prx", "a quoted 'module'.prx"].map { folder.appendingPathComponent($0) }
        for module in modules { try Data("literal module argv fixture\n".utf8).write(to: module) }
        var sceGame = LocalGame(id: "sce", title: "SCE fixture", executablePath: sceExecutable.path,
                                workingDirectory: folder.path, sceModulePaths: modules.map(\.path))
        var selfHeader = sceHeader; selfHeader.replaceSubrange(0..<4, with: [0x4f, 0x15, 0x3d, 0x1d])
        let selfExecutable = folder.appendingPathComponent("SELF guest $(literal) name.bin")
        try selfHeader.write(to: selfExecutable)
        var selfGame = sceGame; selfGame.executablePath = selfExecutable.path
        // Signature routes the protocol; segment/plaintext validation remains the engine's responsibility.
        let modulePayload = payload.replacingOccurrences(of: "\"static_elf64_x86_64\"", with: "\"static_elf64_x86_64\",\"sce_elf64_x86_64\"")
            .replacingOccurrences(of: "\"runtime_abi\":", with: "\"supported_containers\":[\"plain_self\"],\"sce_module_argument\":\"--sce-module\",\"runtime_abi\":")
        let moduleCapabilities = try EngineCapabilities.decode(Data(modulePayload.utf8))
        for target in [sceGame, selfGame] {
            for override: URL? in [nil, separateResources] {
                let (text, status, start) = try await recordRun(target, moduleCapabilities, override)
                try checkOutput(text, status, start, target, moduleCapabilities, override,
                                ["--sce-module", modules[0].path, "--sce-module", modules[1].path])
            }
        }
        let missingCapability = try EngineCapabilities.decode(Data(modulePayload.replacingOccurrences(of:
            "\"sce_module_argument\":\"--sce-module\",", with: "").utf8))
        let unknownCapability = try EngineCapabilities.decode(Data(modulePayload.replacingOccurrences(of:
            "\"--sce-module\"", with: "\"--unknown-module\"").utf8))
        let staticOnlyCapability = try EngineCapabilities.decode(Data(modulePayload.replacingOccurrences(of:
            ",\"sce_elf64_x86_64\"", with: "").utf8))
        let unavailable = folder.appendingPathComponent("removed library.prx")
        let unreadable = folder.appendingPathComponent("unreadable library.prx")
        try Data("unreadable fixture\n".utf8).write(to: unreadable)
        try FileManager.default.setAttributes([.posixPermissions: 0o000], ofItemAtPath: unreadable.path)
        XCTAssertFalse(FileManager.default.isReadableFile(atPath: unreadable.path), "Unreadable-file case needs an unreadable premise.")
        var staticWithModules = game; staticWithModules.sceModulePaths = modules.map(\.path)
        var rejected: [(LocalGame, EngineCapabilities?, String)] = [
            (staticWithModules, nil, "does not advertise loading supplied SCE"),
            (staticWithModules, legacyCapabilities, "does not advertise loading supplied SCE"),
            (sceGame, missingCapability, "does not advertise loading supplied SCE"),
            (sceGame, unknownCapability, "does not advertise loading supplied SCE"),
            (staticWithModules, staticOnlyCapability, "does not advertise loading supplied SCE")
        ]
        rejected.append((staticWithModules, moduleCapabilities, "require an SCE executable"))
        for path in [unavailable.path, unreadable.path, folder.path, "relative-library.prx"] {
            sceGame.sceModulePaths = [path]
            rejected.append((sceGame, moduleCapabilities, "not a readable regular file"))
        }
        for (target, contract, diagnostic) in rejected {
            do {
                _ = try await recordRun(target, contract, separateResources)
                XCTFail("Unsupported attached modules must be rejected before child launch.")
            } catch { XCTAssertTrue(error.localizedDescription.contains(diagnostic), "\(error)") }
            for location in [folder, separateResources] {
                XCTAssertFalse(FileManager.default.fileExists(atPath: location.appendingPathComponent("started").path),
                               "Rejected module routing started an engine child.")
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

    // Contract: inspection retains restrictions, rejects unknown reports, and cancels its started child without returning success.
    // Regression: wrong run flags, swallowed exit126, or cancellation ignored after child startup presents a false result.
    // Existing subprocess coverage launches guests and cannot protect the non-executing inspection protocol.
    func testInspectionProtocolAndRejection() async throws {
        let folder = try directory()
        var game = try guest(in: folder)
        game.sceModulePaths = [folder.appendingPathComponent("missing inspection-only library.prx").path]
        let engine = folder.appendingPathComponent("inspector")
        let capabilities = try EngineCapabilities.decode(Data("""
        {"schema_version":1,"host_architecture":"arm64","guest_architecture":"x86_64","backend":"unicorn","supported_formats":["sce_elf64_x86_64"],"runtime_abi":"sce_sysv","ps5_game_runtime_ready":false}
        """.utf8))
        let report = """
        {"schema_version":1,"event":"inspection","format":"sce_elf64_x86_64","segment_count":2,"relocation_count":1,"has_tls":true,"has_process_parameters":false,"imports":[{"nid":"required-NID","library":"libkernel","module":"libkernel"}],"needed_modules":["libkernel"],"needed_files":[],"unsupported_reasons":["guest TLS unsupported"]}
        """
        // No shell is used by production; the test child checks its literal argv independently.
        let literalPath = "'" + game.executablePath.replacingOccurrences(of: "'", with: "'\\''") + "'"
        let script = "#!/bin/sh\n[ \"$#\" = 2 ] || exit 93\n[ \"$1\" = '--inspect-sce-json' ] || exit 91\n[ \"$2\" = \(literalPath) ] || exit 92\ncat <<'JSON'\n\(report)\nJSON\n"
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
        // Process cancellation is an inspection protocol contract, not evidence of game compatibility.
        // A valid TERM response catches success-after-cancel; ignored TERM exercises owned escalation without pipe-holding descendants.
        let literalReport = "'" + report.replacingOccurrences(of: "'", with: "'\\''") + "'"
        let inspectedGame = game
        for (index, ignoresTERM) in [false, true].enumerated() {
            let ready = folder.appendingPathComponent("inspection-started-\(index)")
            let pidFile = folder.appendingPathComponent("inspection-child-\(index).pid")
            let waiting = """
            #!/bin/sh
            [ "$#" = 2 ] || exit 93
            [ "$1" = '--inspect-sce-json' ] || exit 91
            [ "$2" = \(literalPath) ] || exit 92
            complete_after_term() {
              printf '%s\\n' \(literalReport)
              exit 0
            }
            \(ignoresTERM ? "trap ':' TERM" : "trap complete_after_term TERM")
            printf '%s\\n' "$$" > '\(pidFile.lastPathComponent)' || exit 94
            : > '\(ready.lastPathComponent)' || exit 94
            while :; do :; done
            """
            try Data(waiting.utf8).write(to: engine)
            let operation = Task { try await EngineInspection.inspect(engine: engine, game: inspectedGame, capabilities: capabilities) }
            let clock = ContinuousClock()
            let startupDeadline = clock.now.advanced(by: .seconds(3))
            while !FileManager.default.fileExists(atPath: ready.path), clock.now < startupDeadline {
                try await Task.sleep(for: .milliseconds(10))
            }
            guard FileManager.default.fileExists(atPath: ready.path) else {
                operation.cancel()
                _ = try? await operation.value
                XCTFail("Cancellation control never reached its child-start witness.")
                continue
            }
            let pid = try XCTUnwrap(Int32(try String(contentsOf: pidFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)))
            guard pid > 1, pid != Darwin.getpid() else {
                operation.cancel()
                XCTFail("Invalid owned inspection child PID: \(pid)")
                continue
            }
            XCTAssertEqual(Darwin.kill(pid, 0), 0, "The started fixture must still be alive before cancellation.")
            func childIsGone() -> Bool { Darwin.kill(pid, 0) == -1 && errno == ESRCH }
            var observedGone = false
            defer {
                operation.cancel()
                if !observedGone, Darwin.kill(pid, 0) == 0 { Darwin.kill(pid, SIGKILL) }
            }
            operation.cancel() // Trap installation and PID publication both precede this cancellation.
            let shutdownDeadline = clock.now.advanced(by: .seconds(3))
            while !childIsGone(), clock.now < shutdownDeadline { try await Task.sleep(for: .milliseconds(10)) }
            observedGone = childIsGone()
            XCTAssertTrue(observedGone, "Canceled inspection child remained alive beyond 3s; the 15s inspection timeout must not mask cancellation.")
            if !observedGone {
                // Preserve the failed shutdown observation, then clean up only this test's witnessed child before awaiting the old API.
                Darwin.kill(pid, SIGTERM)
                let cleanupDeadline = clock.now.advanced(by: .milliseconds(500))
                while !childIsGone(), clock.now < cleanupDeadline { try await Task.sleep(for: .milliseconds(10)) }
                if !childIsGone(), Darwin.kill(pid, 0) == 0 { Darwin.kill(pid, SIGKILL) }
            }
            do {
                _ = try await operation.value
                XCTFail("Canceled inspection returned a successful report, including a valid JSON/exit0 TERM response.")
            } catch is CancellationError {
                // Cancellation wins over a valid report or the exit status from terminating a stubborn owned child.
            } catch { XCTFail("Started inspection returned \(error) instead of CancellationError.") }
            observedGone = childIsGone()
            XCTAssertTrue(observedGone, "Inspection completed while its owned child remained present.")
        }
    }

    // Contract: native package acceptance rejects unsafe paths, stale bytes, wrong architectures/closure and a real failed CRT keeper.
    // Regression: trusting manifest architecture text, omitting transitive dependencies, or ignoring validation process failure.
    // Static/argv keepers cannot establish native package integrity. Controls alter only copies; no fake backend or production seam.
    func testFrozenEnginePackageRejectsInvalidCandidates() async throws {
        let cases: [(String, String)] = [
            ("missing manifest", "Engine package path"), ("malformed manifest", "Engine package manifest"),
            ("unknown schema", "Engine package manifest"), ("changed bytes", "Engine package integrity"),
            ("wrong CLI architecture", "Engine package architecture"), ("wrong dylib architecture", "Engine package architecture"),
            ("parent traversal", "Engine package path"), ("escaped symlink", "Engine package path"),
            ("unhashed dependency", "Engine package dependency"), ("external load command", "Engine package dependency"),
            ("unhashed CRT main", "Engine package integrity"), ("unhashed raw CRT", "Engine package integrity"),
            ("unhashed SELF CRT", "Engine package integrity"),
            ("changed validation contract", "Engine package manifest"), ("incorrect CRT certificate", "Engine package validation"),
            ("changed saved identity", "Engine package integrity manifest differs from the saved accepted selection")
        ]
        for (control, diagnostic) in cases {
            let root = try copiedEnginePackage()
            defer { try? FileManager.default.removeItem(at: root) }
            let manifestURL = root.appendingPathComponent("manifest.json")
            let originalManifest = try Data(contentsOf: manifestURL)
            let originalIdentity = SHA256.hash(data: originalManifest).map { String(format: "%02x", $0) }.joined()
            var manifest = try XCTUnwrap(JSONSerialization.jsonObject(with: originalManifest) as? [String: Any])
            var files = try XCTUnwrap(manifest["files"] as? [String: Any])
            func replaceAndRehash(_ path: String, _ bytes: Data) throws {
                try bytes.write(to: root.appendingPathComponent(path))
                files[path] = ["size": bytes.count, "sha256": SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()]
            }
            switch control {
            case "missing manifest": try FileManager.default.removeItem(at: manifestURL)
            case "malformed manifest": try Data("{malformed".utf8).write(to: manifestURL)
            case "unknown schema": manifest["schema_version"] = 2
            case "changed bytes":
                let path = "fixtures/cpu-homebrew.elf"
                var bytes = try Data(contentsOf: root.appendingPathComponent(path)); bytes[bytes.count - 1] ^= 1
                try bytes.write(to: root.appendingPathComponent(path)) // Deliberately retain the old digest.
            case "wrong CLI architecture", "wrong dylib architecture":
                let path = control == "wrong CLI architecture" ? "bin/anyps5_cpu_run" : "lib/libqemu-x86_64-softmmu.dylib"
                var bytes = try Data(contentsOf: root.appendingPathComponent(path))
                XCTAssertEqual(Array(bytes.prefix(4)), [0xcf, 0xfa, 0xed, 0xfe], "Architecture control needs thin little-endian Mach-O.")
                bytes.replaceSubrange(4..<8, with: [7, 0, 0, 1]) // CPU_TYPE_X86_64, with matching digest to reach architecture guard.
                try replaceAndRehash(path, bytes)
            case "parent traversal":
                try FileManager.default.copyItem(at: root.appendingPathComponent("licenses/AnyPS5-LICENSE"),
                                                 to: root.deletingLastPathComponent().appendingPathComponent("outside-license"))
                files["../outside-license"] = files["licenses/AnyPS5-LICENSE"]
            case "escaped symlink":
                let path = "fixtures/SceModuleGuest.prx"
                let originalRoot = URL(fileURLWithPath: try XCTUnwrap(ProcessInfo.processInfo.environment["ANYPS5_ENGINE_PACKAGE"]))
                try FileManager.default.removeItem(at: root.appendingPathComponent(path))
                try FileManager.default.createSymbolicLink(at: root.appendingPathComponent(path),
                                                          withDestinationURL: originalRoot.appendingPathComponent(path))
            case "unhashed dependency":
                files.removeValue(forKey: "lib/libglib-2.0.0.dylib")
                try FileManager.default.removeItem(at: root.appendingPathComponent("lib/libglib-2.0.0.dylib"))
            case "unhashed CRT main", "unhashed raw CRT", "unhashed SELF CRT":
                let path = control == "unhashed CRT main" ? "fixtures/sce-crt/sce-crt-main.elf" :
                    (control == "unhashed raw CRT" ? "fixtures/sce-crt/raw/SceCrtGuest.prx" : "fixtures/sce-crt/plain-self/SceCrtGuest.prx")
                files.removeValue(forKey: path) // Keep the real file: a keeper read failure must not substitute for hash coverage.
            case "external load command":
                let path = "lib/libqemu-x86_64-softmmu.dylib"
                var bytes = try Data(contentsOf: root.appendingPathComponent(path))
                let loadPath = Data("@loader_path/libglib-2.0.0.dylib".utf8)
                let commandsEnd = 32 + (0..<4).reduce(0) { $0 | (Int(bytes[20 + $1]) << (8 * $1)) }
                let range = try XCTUnwrap(bytes.range(of: loadPath, in: 32..<commandsEnd), "Control needs the genuine QEMU GLib load command.")
                var external = Data("/opt/homebrew/lib/glib.dylib".utf8)
                XCTAssertLessThan(external.count, loadPath.count)
                external.append(Data(repeating: 0, count: loadPath.count - external.count))
                bytes.replaceSubrange(range, with: external)
                try replaceAndRehash(path, bytes)
            case "changed validation contract":
                var validation = try XCTUnwrap(manifest["compiled_crt_validation"] as? [String: Any])
                validation["expected_stdout"] = ["arbitrary success"]
                manifest["compiled_crt_validation"] = validation
            case "incorrect CRT certificate", "changed saved identity":
                if control == "changed saved identity" { manifest["checkpoint"] = "changed accepted package" }
                let path = "fixtures/sce-crt/crt-receipt.txt"
                var text = try String(contentsOf: root.appendingPathComponent(path), encoding: .utf8)
                let digest = try XCTUnwrap(text.split(separator: " ").dropFirst(2).first)
                XCTAssertEqual(digest.count, 64)
                let wrongDigest = (digest.first == "0" ? "1" : "0") + String(digest.dropFirst())
                text = text.replacingOccurrences(of: String(digest), with: wrongDigest)
                try replaceAndRehash(path, Data(text.utf8)) // Hash-valid package; real compiled source certificate now disagrees.
                // Restored identity must reject before this genuine keeper fault, not after fixtures execute.
            default: XCTFail("Unhandled negative control: \(control)")
            }
            if control != "missing manifest" && control != "malformed manifest" {
                manifest["files"] = files
                try JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys]).write(to: manifestURL)
            }
            let selected = control == "malformed manifest" ? manifestURL :
                (control == "unknown schema" ? root.appendingPathComponent("bin/anyps5_cpu_run") : root)
            do {
                _ = try await EnginePackage.accept(selectedURL: selected,
                                                   expectedManifestSHA256: control == "changed saved identity" ? originalIdentity : nil)
                XCTFail("Invalid package accepted: \(control)")
            } catch {
                XCTAssertTrue(error.localizedDescription.hasPrefix(diagnostic), "\(control) reached the wrong guard: \(error)")
            }
        }
    }

    // Contract: MainActor can stop an intact accepted launch during preparation; no guest starts and the runner remains reusable.
    // Regression: stop is ignored during the hash scan or canceled preparation leaves the session reserved.
    // Invalid-package rows only protect rejection, not cancellation of valid preparation. This uses the real package and runner.
    @MainActor
    func testStopDuringAcceptedPreparationPreventsGuestAndAllowsRelaunch() async throws {
        let root = try copiedEnginePackage()
        let package = try await EnginePackage.accept(selectedURL: root)
        let game = LocalGame(id: "prepared-homebrew", title: "Prepared homebrew",
                             executablePath: root.appendingPathComponent("fixtures/cpu-homebrew.elf").path,
                             workingDirectory: root.path)
        let runner = EngineRunner()
        let stream = try runner.run(engine: package.executableURL, game: game, acceptedPackage: package)
        runner.stop() // The UI actor acts immediately after the factory, before yielding to stream consumption.
        var received = 0
        do {
            for try await _ in stream { received += 1 }
            XCTFail("Stopped preparation must finish with cancellation before any guest event.")
        } catch is CancellationError {
            // A valid package canceled before startup must not be reported as an integrity failure or a guest exit.
        } catch { XCTFail("Wrong preparation failure: \(error)") }
        XCTAssertEqual(received, 0, "Canceled preparation started a guest or published a guest exit.")

        var output = Data(); var status: Int32?
        for try await event in try runner.run(engine: package.executableURL, game: game, acceptedPackage: package) {
            switch event { case .started: break; case .output(let bytes): output.append(bytes); case .exited(let code): status = code }
        }
        XCTAssertEqual(status, 0, "Canceled preparation must release the reserved session for a fresh launch.")
        XCTAssertTrue(String(decoding: output, as: UTF8.self)
            .contains("homebrew primes=168 sum=76127 buffer_crc32=2511520486 tls=ok bss=ok\n"),
                      "Relaunch did not execute the real arithmetic/TLS fixture: \(String(decoding: output, as: UTF8.self))")
    }

    // Contract: the actual engine preserves static results/inspection and accepts a relocated native package before routing real modules.
    // Regression: accepting stale runtime bytes, losing supplied modules, or dropping qualified inspection service families.
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
            switch event { case .started: break; case .output(let bytes): output.append(bytes); case .exited(let code): exit = code }
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
        if environment["ANYPS5_ENGINE_PACKAGE"] != nil {
            let root = try copiedEnginePackage()
            let package = try await EnginePackage.accept(selectedURL: root)
            XCTAssertEqual(package.executableURL, root.appendingPathComponent("bin/anyps5_cpu_run"))
            XCTAssertEqual(package.sourceCommit, "5c9af66412d87c99651e87462ce6fee0f12f4cd2")
            XCTAssertEqual(package.engineCommit, "f82b6dd02638cfe76afa16fb54a1199daff21242")
            XCTAssertEqual(package.capabilities.backend, "Modern QEMU TCG x86-64 dynamic translation")
            XCTAssertEqual(package.capabilities.sceModuleArgument, "--sce-module")
            XCTAssertFalse(package.capabilities.ps5GameRuntimeReady)
            let unrelated = try directory().appendingPathComponent("unrelated resource $(literal) directory")
            try FileManager.default.createDirectory(at: unrelated, withIntermediateDirectories: false)
            let moduleMain = root.appendingPathComponent("fixtures/sce-module-main.elf")
            let dependency = root.appendingPathComponent("fixtures/SceModuleGuest.prx")
            var moduleGame = LocalGame(id: "compiled-module", title: "Compiled module routing", executablePath: moduleMain.path,
                                      workingDirectory: unrelated.path, sceModulePaths: [dependency.path])
            for withDependency in [true, false] {
                moduleGame.sceModulePaths = withDependency ? [dependency.path] : []
                var bytes = Data(); var status: Int32?
                // Accepted proof supplies capabilities even when the caller has no cached probe.
                for try await event in try EngineRunner().run(engine: package.executableURL, game: moduleGame,
                                                         resourceDirectory: unrelated, acceptedPackage: package) {
                    switch event { case .started: break; case .output(let chunk): bytes.append(chunk); case .exited(let code): status = code }
                }
                let events = try String(decoding: bytes, as: UTF8.self).split(separator: "\n").map {
                    try XCTUnwrap(JSONSerialization.jsonObject(with: Data($0.utf8)) as? [String: Any])
                }
                XCTAssertEqual(events.compactMap { $0["event"] as? String }, withDependency ? ["startup", "guest_exit"] : ["error"])
                if withDependency {
                    // SceModuleMain.c independently requires argc6; production launcher supplies argc1 and returns81.
                    // This establishes routing/translated entry/exit. Full arithmetic/TLS/CRT proof is accept's packaged keeper.
                    XCTAssertEqual(status, 81)
                    XCTAssertEqual(events.last?["exit_code"] as? Int, 81)
                    XCTAssertEqual(events.first?["host_architecture"] as? String, "arm64")
                    XCTAssertGreaterThan((events.first?["entry"] as? NSNumber)?.uint64Value ?? 0, 0)
                } else {
                    XCTAssertEqual(status, 126)
                    XCTAssertEqual(events.first?["code"] as? String, "unsupported_executable")
                    XCTAssertTrue((events.first?["message"] as? String)?.contains("DT_NEEDED guest module loading is unsupported") == true)
                }
            }
            // Acceptance cannot authorize different bytes or transfer its live capabilities to a different engine.
            let fixture = root.appendingPathComponent("fixtures/cpu-homebrew.elf")
            let original = try Data(contentsOf: fixture)
            var changed = original; changed[changed.count - 1] ^= 1
            try changed.write(to: fixture)
            // Hash work belongs to preparation after the factory returns, and failure must precede all guest events.
            // This phase assertion fails the former synchronous full scan without adding a timing threshold or a test hook.
            func rejectPreparedRun(_ selected: URL, _ target: LocalGame, _ diagnostic: String) async throws {
                let stream: AsyncThrowingStream<EngineEvent, Error>
                do { stream = try EngineRunner().run(engine: selected, game: target, acceptedPackage: package) }
                catch {
                    XCTFail("Full package verification blocked/failed inside the stream factory: \(error)")
                    throw error
                }
                var received = 0
                do {
                    for try await _ in stream { received += 1 }
                    XCTFail("Changed package or mismatched engine reached guest execution.")
                } catch { XCTAssertTrue(error.localizedDescription.hasPrefix(diagnostic), "\(error)") }
                XCTAssertEqual(received, 0, "Integrity failure must precede guest output, startup and exit.")
            }
            try await rejectPreparedRun(package.executableURL, moduleGame, "Engine package integrity")
            try original.write(to: fixture)
            try await rejectPreparedRun(engine, game, "Engine package integrity belongs to a different selected engine")
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
