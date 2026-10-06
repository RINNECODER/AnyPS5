import Foundation
import CryptoKit
import Darwin

/// Acceptance of a local diagnostic package is integrity/fixture evidence, not game compatibility.
public struct EnginePackage: Sendable {
    public let rootURL: URL
    public let executableURL: URL
    public let capabilities: EngineCapabilities
    public let manifestSHA256: String
    public let sourceCommit: String
    public let engineCommit: String

    private struct FileRecord: Decodable, Sendable { let sha256: String; let size: UInt64 }
    private struct Revision: Decodable, Sendable { let commit: String; let tree: String; let clean: Bool }
    private struct Validation: Decodable, Sendable { let argv: [String]; let expected_stdout: [String] }
    private struct Manifest: Decodable, Sendable {
        let schema_version: Int
        let backend: String
        let source_revisions: [String: Revision]
        let files: [String: FileRecord]
        let compiled_crt_validation: Validation
    }
    private struct NativeImage { let dependencies: [String]; let rpaths: [String] }
    private static let enginePath = "bin/anyps5_cpu_run"
    private static let keeperArguments = ["bin/anyps5_sce_modules_tests", "fixtures/sce-module-main.elf",
                                          "fixtures/SceModuleGuest.prx", "fixtures/sce-crt/crt-receipt.txt"]
    private static let keeperOutput = [
        "PASS compiled SCE module graph calls, objects, TLS, independent prime/Adler results, dependency-only lifecycle, and strict failures",
        "PASS original ELF/SELF certification, chunked source identity, compiled CRT ordering, and certificate preflight failures"
    ]

    /// Accept a package folder, its manifest, or its engine executable. No saved selection is changed here.
    public static func accept(selectedURL: URL, expectedManifestSHA256: String? = nil) async throws -> Self {
        let selected = selectedURL.standardizedFileURL
        let root: URL
        if selected.lastPathComponent == "manifest.json" { root = selected.deletingLastPathComponent() }
        else if selected.lastPathComponent == "anyps5_cpu_run", selected.deletingLastPathComponent().lastPathComponent == "bin" {
            root = selected.deletingLastPathComponent().deletingLastPathComponent()
        } else { root = selected }
        let canonicalRoot = root.resolvingSymlinksInPath()
        let (manifest, identity) = try await background { try inspect(root: canonicalRoot) }
        if let expectedManifestSHA256, identity != expectedManifestSHA256 {
            throw LauncherError("Engine package integrity manifest differs from the saved accepted selection.")
        }
        let executable = try contained(enginePath, root: canonicalRoot)
        let environment = controlledEnvironment
        let capabilities = try await EngineCapabilities.probe(executable, environment: environment,
                                                               currentDirectory: canonicalRoot)
        guard capabilities.backend == "Modern QEMU TCG x86-64 dynamic translation",
              capabilities.cpuProfile == "Haswell",
              Set(["AVX", "AVX2", "F16C", "FMA"]).isSubset(of: Set(capabilities.supportedInstructionFamilies ?? [])),
              capabilities.sceModuleArgument == "--sce-module", capabilities.resourceRootArgument == "--resource-root",
              capabilities.supportedFormats.contains("static_elf64_x86_64"),
              capabilities.supportedFormats.contains("sce_elf64_x86_64"),
              capabilities.supportedContainers?.contains("plain_self") == true,
              capabilities.runtimeABIs?.contains("sce_sysv") == true, !capabilities.ps5GameRuntimeReady else {
            throw LauncherError("Engine package capabilities do not match the frozen modern TCG diagnostic contract.")
        }
        try await background {
            let cwd = FileManager.default.temporaryDirectory.appendingPathComponent("AnyPS5 package validation " + UUID().uuidString)
            try FileManager.default.createDirectory(at: cwd, withIntermediateDirectories: false)
            defer { try? FileManager.default.removeItem(at: cwd) }
            let arithmetic = try execute(executable, arguments: [try contained("fixtures/cpu-homebrew.elf", root: canonicalRoot).path], cwd: cwd)
            guard arithmetic == "homebrew primes=168 sum=76127 buffer_crc32=2511520486 tls=ok bss=ok\n" else {
                throw LauncherError("Engine package validation failed the independent arithmetic/TLS fixture.")
            }
            let argv = try keeperArguments.map { try contained($0, root: canonicalRoot).path }
            let crt = try execute(URL(fileURLWithPath: argv[0]), arguments: Array(argv.dropFirst()), cwd: cwd)
            guard crt == keeperOutput.joined(separator: "\n") + "\n" else {
                throw LauncherError("Engine package validation failed the compiled module/CRT fixture.")
            }
            let (_, after) = try inspect(root: canonicalRoot)
            guard after == identity else { throw LauncherError("Engine package integrity changed during acceptance.") }
        }
        return Self(rootURL: canonicalRoot, executableURL: executable, capabilities: capabilities,
                    manifestSHA256: identity, sourceCommit: manifest.source_revisions["AnyPS5"]!.commit,
                    engineCommit: manifest.source_revisions["3rdparty/anyps5-tcg"]!.commit)
    }

    /// Recheck the same accepted bytes before a launch; capabilities cannot migrate to another engine.
    public func verifyIntegrity(for engine: URL) async throws {
        let cancellation = EnginePreparationCancellation()
        try await withTaskCancellationHandler {
            try Task.checkCancellation()
            try await Self.background {
                try verifyIntegrityOnWorker(for: engine, checkingCancellation: cancellation.check)
            }
            try Task.checkCancellation()
        } onCancel: {
            cancellation.cancel()
        }
    }

    // The runner already owns a background worker, so it must not dispatch Process.run/wait
    // across an await. Keep the complete scan and process lifecycle on that worker.
    func verifyIntegrityOnWorker(for engine: URL, checkingCancellation: () throws -> Void) throws {
        try checkingCancellation()
        guard engine.standardizedFileURL == executableURL else {
            throw LauncherError("Engine package integrity belongs to a different selected engine.")
        }
        let (_, identity) = try Self.inspect(root: rootURL, checkingCancellation: checkingCancellation)
        try checkingCancellation()
        guard identity == manifestSHA256 else { throw LauncherError("Engine package integrity manifest changed after acceptance.") }
    }

    // Frozen execution must not load injected/search-path libraries from the caller's environment.
    public static var controlledEnvironment: [String: String] {
        ProcessInfo.processInfo.environment.filter { !$0.key.hasPrefix("DYLD_") && !$0.key.hasPrefix("LD_") }
    }

    private static func inspect(root: URL, checkingCancellation: () throws -> Void = {}) throws -> (Manifest, String) {
        try checkingCancellation()
        let manifestURL = try contained("manifest.json", root: root)
        let attributes = try FileManager.default.attributesOfItem(atPath: manifestURL.path)
        guard ((attributes[.size] as? NSNumber)?.uint64Value ?? UInt64.max) <= 1_048_576 else {
            throw LauncherError("Engine package manifest is too large.")
        }
        let data = try Data(contentsOf: manifestURL)
        let manifest: Manifest
        do { manifest = try JSONDecoder().decode(Manifest.self, from: data) }
        catch { throw LauncherError("Engine package manifest is missing or malformed: \(error.localizedDescription)") }
        guard manifest.schema_version == 1, manifest.backend == "TCG", !manifest.files.isEmpty, manifest.files.count <= 128,
              manifest.compiled_crt_validation.argv == keeperArguments,
              manifest.compiled_crt_validation.expected_stdout == keeperOutput else {
            throw LauncherError("Engine package manifest has an unsupported schema or validation contract.")
        }
        for key in ["AnyPS5", "3rdparty/anyps5-tcg", "3rdparty/unicorn"] {
            guard let revision = manifest.source_revisions[key], revision.clean,
                  validHex(revision.commit, count: 40), validHex(revision.tree, count: 40) else {
                throw LauncherError("Engine package manifest lacks a clean source revision for \(key).")
            }
        }
        for (path, record) in manifest.files {
            try checkingCancellation()
            guard validHex(record.sha256, count: 64), record.size <= 268_435_456 else {
                throw LauncherError("Engine package integrity has an invalid file record: \(path)")
            }
            let url = try contained(path, root: root)
            let file = try FileHandle(forReadingFrom: url)
            defer { try? file.close() }
            var hash = SHA256(); var size: UInt64 = 0
            while let chunk = try file.read(upToCount: 65_536), !chunk.isEmpty {
                try checkingCancellation()
                size += UInt64(chunk.count)
                guard size <= record.size else { throw LauncherError("Engine package integrity size mismatch: \(path)") }
                hash.update(data: chunk)
            }
            guard size == record.size, hex(hash.finalize()) == record.sha256 else {
                throw LauncherError("Engine package integrity hash/size mismatch: \(path)")
            }
        }
        let crtFiles = ["fixtures/sce-crt/sce-crt-main.elf", "fixtures/sce-crt/raw/SceCrtGuest.prx",
                        "fixtures/sce-crt/plain-self/SceCrtGuest.prx"]
        for path in keeperArguments + [enginePath, "fixtures/cpu-homebrew.elf"] + crtFiles {
            guard manifest.files[path] != nil else { throw LauncherError("Engine package integrity omits a required file: \(path)") }
        }
        // The native keeper reads paths from this receipt. Constrain those paths before execution.
        let receipt = try String(contentsOf: contained(keeperArguments[3], root: root), encoding: .utf8)
        let records = receipt.split(whereSeparator: \.isNewline).map { $0.split(whereSeparator: \.isWhitespace) }
        guard records.count == 2, records[0].count == 13, records[1].count == 13,
              records[0][0] == "elf", records[0][1] == "raw/SceCrtGuest.prx",
              records[1][0] == "plain_self", records[1][1] == "plain-self/SceCrtGuest.prx" else {
            throw LauncherError("Engine package manifest has an unsupported CRT receipt path contract.")
        }
        for executable in [enginePath, keeperArguments[0]] {
            try checkingCancellation()
            guard FileManager.default.isExecutableFile(atPath: try contained(executable, root: root).path) else {
                throw LauncherError("Engine package architecture requires an executable native binary: \(executable)")
            }
            try closure(executable, root: root, files: manifest.files)
        }
        return (manifest, hex(SHA256.hash(data: data)))
    }

    private static func contained(_ path: String, root: URL) throws -> URL {
        let parts = path.components(separatedBy: "/")
        guard !path.hasPrefix("/"), !path.contains("\0"), !parts.isEmpty,
              parts.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
            throw LauncherError("Engine package path is not a safe relative file: \(path)")
        }
        var candidate = root
        for part in parts {
            candidate.appendPathComponent(part)
            let values: URLResourceValues
            do { values = try candidate.resourceValues(forKeys: [.isSymbolicLinkKey]) }
            catch { throw LauncherError("Engine package path is unavailable: \(path)") }
            guard values.isSymbolicLink != true else { throw LauncherError("Engine package path contains a symbolic link: \(path)") }
        }
        guard candidate.resolvingSymlinksInPath().path.hasPrefix(root.path + "/"),
              try candidate.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true else {
            throw LauncherError("Engine package path is not a contained regular file: \(path)")
        }
        return candidate
    }

    private static func closure(_ executable: String, root: URL, files: [String: FileRecord]) throws {
        var visited = Set<String>()
        func visit(_ relative: String, inheritedRpaths: [String]) throws {
            guard visited.insert(relative).inserted else { return }
            guard files[relative] != nil else { throw LauncherError("Engine package dependency is not hash-covered: \(relative)") }
            let url = try contained(relative, root: root)
            let image = try nativeImage(url, executable: relative == executable)
            func expand(_ value: String) throws -> URL {
                let result: URL
                if value.hasPrefix("@loader_path/") { result = url.deletingLastPathComponent().appendingPathComponent(String(value.dropFirst(13))) }
                else if value.hasPrefix("@executable_path/") {
                    result = root.appendingPathComponent(executable).deletingLastPathComponent().appendingPathComponent(String(value.dropFirst(17)))
                } else { throw LauncherError("Engine package dependency uses an external or unsupported load path: \(value)") }
                let normalized = result.standardizedFileURL
                guard normalized.path.hasPrefix(root.path + "/") else { throw LauncherError("Engine package dependency escapes its package: \(value)") }
                return normalized
            }
            let rpaths = try image.rpaths.map { try expand($0).path } + inheritedRpaths
            for dependency in image.dependencies {
                if dependency.hasPrefix("/usr/lib/") || dependency.hasPrefix("/System/Library/") {
                    guard URL(fileURLWithPath: dependency).standardizedFileURL.path == dependency else {
                        throw LauncherError("Engine package dependency has a noncanonical system path.")
                    }
                    continue
                }
                let candidate: URL
                if dependency.hasPrefix("@rpath/") {
                    let suffix = String(dependency.dropFirst(7))
                    guard let match = rpaths.map({ URL(fileURLWithPath: $0).appendingPathComponent(suffix).standardizedFileURL })
                        .first(where: { FileManager.default.fileExists(atPath: $0.path) }) else {
                        throw LauncherError("Engine package dependency cannot resolve \(dependency).")
                    }
                    candidate = match
                } else { candidate = try expand(dependency) }
                guard candidate.path.hasPrefix(root.path + "/") else { throw LauncherError("Engine package dependency escapes its package.") }
                let next = String(candidate.path.dropFirst(root.path.count + 1))
                try visit(next, inheritedRpaths: rpaths)
            }
        }
        try visit(executable, inheritedRpaths: [])
    }

    // Read actual thin Mach-O bytes; manifest otool text is provenance, not dependency evidence.
    private static func nativeImage(_ url: URL, executable: Bool) throws -> NativeImage {
        let data = try Data(contentsOf: url, options: .mappedIfSafe)
        func word(_ offset: Int) throws -> UInt32 {
            guard offset >= 0, offset <= data.count - 4 else { throw LauncherError("Engine package architecture has truncated Mach-O metadata.") }
            return (0..<4).reduce(0) { $0 | (UInt32(data[offset + $1]) << (8 * $1)) }
        }
        guard data.count >= 32, try word(0) == 0xfeedfacf, try word(4) == 0x0100000c,
              try word(12) == (executable ? 2 : 6) else {
            throw LauncherError("Engine package architecture requires thin ARM64 Mach-O: \(url.lastPathComponent)")
        }
        let count = Int(try word(16)), bytes = Int(try word(20))
        guard count <= 4096, bytes <= data.count - 32 else { throw LauncherError("Engine package architecture has invalid load commands.") }
        var offset = 32; var dependencies: [String] = []; var rpaths: [String] = []
        var hasMacOSVersion = false
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let currentOS = UInt32(os.majorVersion << 16 | os.minorVersion << 8 | os.patchVersion)
        for _ in 0..<count {
            let command = try word(offset), size = Int(try word(offset + 4))
            guard size >= 8, size % 8 == 0, size <= 32 + bytes - offset else {
                throw LauncherError("Engine package architecture has invalid load-command bounds.")
            }
            func string(minimum: Int) throws -> String {
                guard size >= minimum else { throw LauncherError("Engine package dependency has a truncated load command.") }
                let start = Int(try word(offset + 8))
                guard start >= minimum, start < size,
                      let end = data[(offset + start)..<(offset + size)].firstIndex(of: 0),
                      let value = String(data: data[(offset + start)..<end], encoding: .utf8), !value.isEmpty else {
                    throw LauncherError("Engine package dependency has an invalid load-command string.")
                }
                return value
            }
            switch command {
            case 0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023: dependencies.append(try string(minimum: 24))
            case 0x8000001c: rpaths.append(try string(minimum: 12))
            case 0xe:
                guard try string(minimum: 12) == "/usr/lib/dyld" else {
                    throw LauncherError("Engine package dependency requires the system dynamic linker.")
                }
            case 0x27: throw LauncherError("Engine package dependency embeds an unsupported dyld environment.")
            case 0x32:
                hasMacOSVersion = true
                guard size >= 24, try word(offset + 8) == 1, try word(offset + 12) <= currentOS else {
                    throw LauncherError("Engine package architecture requires a compatible macOS deployment target.")
                }
            case 0x24:
                hasMacOSVersion = true
                guard size >= 16, try word(offset + 8) <= currentOS else {
                    throw LauncherError("Engine package architecture requires a compatible macOS deployment target.")
                }
            case 0x25, 0x2f, 0x30: throw LauncherError("Engine package architecture targets an unsupported Apple platform.")
            default: break // LC_ID_DYLIB is identity, not a loaded dependency.
            }
            offset += size
        }
        guard hasMacOSVersion else { throw LauncherError("Engine package architecture lacks a macOS deployment target.") }
        guard offset == 32 + bytes else { throw LauncherError("Engine package architecture has inconsistent load-command size.") }
        return NativeImage(dependencies: dependencies, rpaths: rpaths)
    }

    private static func execute(_ executable: URL, arguments: [String], cwd: URL) throws -> String {
        let process = Process(); let pipe = Pipe()
        process.executableURL = executable; process.arguments = arguments; process.currentDirectoryURL = cwd
        process.environment = controlledEnvironment
        let stderrURL = cwd.appendingPathComponent("validation stderr " + UUID().uuidString)
        guard FileManager.default.createFile(atPath: stderrURL.path, contents: nil) else {
            throw LauncherError("Engine package validation could not capture diagnostics.")
        }
        let stderr = try FileHandle(forWritingTo: stderrURL)
        defer { try? stderr.close(); try? FileManager.default.removeItem(at: stderrURL) }
        // A file drains stderr independently: a full stderr pipe cannot deadlock stdout reading.
        process.standardInput = FileHandle.nullDevice; process.standardOutput = pipe; process.standardError = stderr
        let timeout = DispatchWorkItem {
            if process.isRunning {
                process.terminate()
                DispatchQueue.global().asyncAfter(deadline: .now() + 2) {
                    if process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
                }
            }
        }
        var started = false
        defer { timeout.cancel(); try? pipe.fileHandleForReading.close(); try? pipe.fileHandleForWriting.close() }
        do {
            try process.run(); started = true; try pipe.fileHandleForWriting.close()
            DispatchQueue.global().asyncAfter(deadline: .now() + 30, execute: timeout)
            var output = Data()
            while let chunk = try pipe.fileHandleForReading.read(upToCount: 4096), !chunk.isEmpty {
                output.append(chunk)
                guard output.count <= 1_048_576 else { throw LauncherError("Engine package validation returned excessive output.") }
            }
            process.waitUntilExit()
            try stderr.close()
            let stderrSize = (try FileManager.default.attributesOfItem(atPath: stderrURL.path)[.size] as? NSNumber)?.uint64Value ?? UInt64.max
            guard stderrSize <= 1_048_576 else { throw LauncherError("Engine package validation returned excessive diagnostics.") }
            let diagnostics = try Data(contentsOf: stderrURL)
            guard process.terminationReason == .exit, process.terminationStatus == 0 else {
                throw LauncherError("Engine package validation failed (exit \(process.terminationStatus)): \(String(decoding: output + diagnostics, as: UTF8.self))")
            }
            return String(decoding: output, as: UTF8.self)
        } catch {
            if started, process.isRunning {
                process.terminate()
                DispatchQueue.global().asyncAfter(deadline: .now() + 2) {
                    if process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
                }
                process.waitUntilExit()
            }
            throw error
        }
    }

    private static func background<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                do { continuation.resume(returning: try operation()) } catch { continuation.resume(throwing: error) }
            }
        }
    }
    private static func validHex(_ value: String, count: Int) -> Bool {
        value.utf8.count == count && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    private static func hex<S: Sequence>(_ bytes: S) -> String where S.Element == UInt8 {
        bytes.map { String(format: "%02x", $0) }.joined()
    }
}

// Shared by package scans and inspection preparation. Cancellation and Process.run are
// serialized so a canceled preparation cannot race its final check and start a child.
final class EnginePreparationCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false

    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
    func check() throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw CancellationError() }
    }
    func start(_ operation: () throws -> Void) throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw CancellationError() }
        try operation()
    }
}
