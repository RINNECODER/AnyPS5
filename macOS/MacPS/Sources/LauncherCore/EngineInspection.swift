import Foundation
import Darwin

/// The engine parses the guest without executing it. A parsed file is not a playable game.
public struct EngineInspection: Decodable, Sendable {
    public struct Import: Decodable, Sendable {
        public let nid: String
        public let library: String
        public let module: String
    }

    public let schemaVersion: Int
    public let event: String
    public let format: String
    public let containerFormat: String?
    public let normalizationNotes: [String]?
    public let segmentCount: Int
    public let relocationCount: Int
    public let hasTLS: Bool
    public let hasProcessParameters: Bool
    public let imports: [Import]
    public let neededModules: [String]
    public let neededFiles: [String]
    public let unsupportedReasons: [String]

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", event, format, imports
        case containerFormat = "container_format", normalizationNotes = "normalization_notes"
        case segmentCount = "segment_count", relocationCount = "relocation_count"
        case hasTLS = "has_tls", hasProcessParameters = "has_process_parameters"
        case neededModules = "needed_modules", neededFiles = "needed_files"
        case unsupportedReasons = "unsupported_reasons"
    }

    public var summary: String {
        var lines = ["Parsed \(format): \(segmentCount) segments, \(relocationCount) relocations.",
                     "Guest TLS: \(hasTLS ? "present" : "absent"). Process parameters: \(hasProcessParameters ? "present" : "absent")."]
        if let containerFormat { lines.append("Container: \(containerFormat)") }
        lines += normalizationNotes ?? []
        if !neededModules.isEmpty { lines.append("Modules: \(neededModules.joined(separator: ", "))") }
        if !neededFiles.isEmpty { lines.append("Needed files: \(neededFiles.joined(separator: ", "))") }
        lines.append("Imports (\(imports.count)):")
        lines += imports.prefix(100).map { "  \($0.module) / \($0.library) / \($0.nid)" }
        if imports.count > 100 { lines.append("  … \(imports.count - 100) more imports") }
        lines += unsupportedReasons.isEmpty ? ["No parser restrictions reported. Runtime compatibility is still unproven."]
            : ["Engine restrictions:"] + unsupportedReasons.map { "  \($0)" }
        lines.append("Inspection does not execute the game or verify gameplay.")
        return lines.joined(separator: "\n")
    }

    public static func inspect(engine: URL, game: LocalGame, capabilities: EngineCapabilities, acceptedPackage: EnginePackage? = nil) async throws -> Self {
        try Task.checkCancellation()
        if let acceptedPackage { try await acceptedPackage.verifyIntegrity(for: engine) }
        try Task.checkCancellation()
        let capabilities = acceptedPackage?.capabilities ?? capabilities
        guard capabilities.supportedFormats.contains("sce_elf64_x86_64") else {
            throw LauncherError("This engine does not advertise SCE ELF inspection support.")
        }
        try EngineRunner.validate(engine: engine, game: game, capabilities: capabilities)
        let cancellation = InspectionOperation()
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            let result: Self = try await withCheckedThrowingContinuation { continuation in
                DispatchQueue.global(qos: .userInitiated).async {
                    let child = Process()
                    let pipe = Pipe()
                    child.executableURL = engine
                    if acceptedPackage != nil { child.environment = EnginePackage.controlledEnvironment }
                    child.arguments = ["--inspect-sce-json", game.executablePath]
                    child.currentDirectoryURL = URL(fileURLWithPath: game.workingDirectory, isDirectory: true)
                    child.standardInput = FileHandle.nullDevice
                    child.standardOutput = pipe
                    child.standardError = pipe
                    let timeout = DispatchWorkItem { cancellation.terminate() }
                    var started = false
                    defer {
                        timeout.cancel()
                        cancellation.complete(child)
                        try? pipe.fileHandleForReading.close()
                        try? pipe.fileHandleForWriting.close()
                    }
                    do {
                        try cancellation.start(child)
                        started = true
                        try? pipe.fileHandleForWriting.close()
                        DispatchQueue.global().asyncAfter(deadline: .now() + 15, execute: timeout)
                        var data = Data()
                        while let bytes = try pipe.fileHandleForReading.read(upToCount: 4096), !bytes.isEmpty {
                            data.append(bytes)
                            if data.count > 4_194_304 { throw LauncherError("The engine returned excessive inspection output.") }
                        }
                        child.waitUntilExit()
                        cancellation.complete(child)
                        timeout.cancel()
                        try cancellation.check()
                        guard child.terminationStatus == 0 else {
                            let diagnostic = String(decoding: data.prefix(16_384), as: UTF8.self)
                            throw LauncherError("Engine inspection failed (exit \(child.terminationStatus)).\n\(diagnostic)")
                        }
                        let result = try JSONDecoder().decode(Self.self, from: data)
                        guard result.schemaVersion == 1, result.event == "inspection", result.format == "sce_elf64_x86_64" else {
                            throw LauncherError("The engine returned an unsupported inspection protocol.")
                        }
                        try cancellation.check()
                        continuation.resume(returning: result)
                    } catch {
                        timeout.cancel()
                        cancellation.terminate()
                        if started { child.waitUntilExit() }
                        cancellation.complete(child)
                        let failure: Error = cancellation.isCancelled ? CancellationError() : error
                        continuation.resume(throwing: failure)
                    }
                }
            }
            try Task.checkCancellation()
            return result
        } onCancel: {
            cancellation.cancel()
        }
    }
}

/// Owns only this inspection's child. Start/registration and cancellation share one lock,
/// and delayed escalation is disarmed after the launching worker finishes waiting.
private final class InspectionOperation: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var child: Process?
    private var escalation: DispatchWorkItem?

    var isCancelled: Bool {
        lock.lock(); defer { lock.unlock() }
        return cancelled
    }

    func check() throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw CancellationError() }
    }

    func start(_ process: Process) throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw CancellationError() }
        try process.run()
        child = process
    }

    func cancel() {
        lock.lock(); defer { lock.unlock() }
        cancelled = true
        terminateLocked()
    }

    func terminate() {
        lock.lock(); defer { lock.unlock() }
        terminateLocked()
    }

    private func terminateLocked() {
        guard let child, child.isRunning else { return }
        child.terminate()
        guard escalation == nil else { return }
        let escalation = DispatchWorkItem { [weak self, child] in
            guard let self else { return }
            self.lock.lock(); defer { self.lock.unlock() }
            // The process must still be the active, unreleased child owned by this operation.
            if self.child === child, child.isRunning { Darwin.kill(child.processIdentifier, SIGKILL) }
        }
        self.escalation = escalation
        DispatchQueue.global().asyncAfter(deadline: .now() + 2, execute: escalation)
    }

    func complete(_ process: Process) {
        lock.lock(); defer { lock.unlock() }
        guard child === process else { return }
        escalation?.cancel()
        escalation = nil
        child = nil
    }
}
