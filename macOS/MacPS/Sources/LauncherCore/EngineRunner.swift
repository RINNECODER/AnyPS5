import Foundation
import Darwin

public enum EngineEvent: Sendable {
    case started(pid: Int32, executable: String, arguments: [String], workingDirectory: String)
    case output(Data)
    case exited(Int32)
}

/// AnyPS5's CLI receives a guest path as one argument and resolves resources from the chosen directory.
public final class EngineRunner: @unchecked Sendable {
    private let lock = NSLock()
    private var process: Process?
    private var stopRequested = false

    public init() {}

    public static func validate(engine: URL, game: LocalGame, capabilities: EngineCapabilities? = nil) throws {
        guard FileManager.default.isExecutableFile(atPath: engine.path) else {
            throw LauncherError("Choose a built AnyPS5 runtime executable in Engine settings.")
        }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: game.workingDirectory, isDirectory: &isDirectory), isDirectory.boolValue else {
            throw LauncherError("The game's resource folder is unavailable. Reconnect its drive or choose another folder.")
        }
        let input = URL(fileURLWithPath: game.executablePath)
        let handle: FileHandle
        do { handle = try FileHandle(forReadingFrom: input) }
        catch { throw LauncherError("The local executable is unavailable: \(error.localizedDescription)") }
        defer { try? handle.close() }
        let header = try handle.read(upToCount: 64) ?? Data()
        let magic = Array(header.prefix(4))
        if header.count >= 64, magic == [0x4f, 0x15, 0x3d, 0x1d] || magic == [0x54, 0x14, 0xf5, 0xee] {
            guard capabilities?.supportedContainers?.contains("plain_self") == true,
                  capabilities?.supportedFormats.contains("sce_elf64_x86_64") == true else {
                throw LauncherError("This engine does not advertise plaintext SELF loading. Select a supported engine or a verified extracted ELF.")
            }
            // The engine validates every SELF segment. A signature alone cannot establish plaintext.
            return
        }
        guard header.count >= 64, Array(header.prefix(7)) == [0x7f, 0x45, 0x4c, 0x46, 2, 1, 1],
              header[18] == 62, header[19] == 0 else {
            throw LauncherError("AnyPS5 needs a supported x86-64 ELF or plaintext SELF executable. FFPFSC, exFAT, PKG and archives cannot be launched directly.")
        }
        // Unadvertised engines use the static checkpoint contract; SCE support requires a capability.
        if let capabilities, !capabilities.supportedFormats.contains("static_elf64_x86_64") && !capabilities.supportedFormats.contains("sce_elf64_x86_64") {
            throw LauncherError("This engine does not advertise a supported ELF input format.")
        }
        if capabilities?.supportedFormats.contains("sce_elf64_x86_64") != true {
            guard header[16] == 2, header[17] == 0, [0, 3].contains(header[7]) else {
                throw LauncherError("This CPU checkpoint accepts static Linux/System V ET_EXEC ELF inputs. The PS5 game loader is still in development.")
            }
        }
    }

    public func run(engine: URL, game: LocalGame, capabilities: EngineCapabilities? = nil,
                    resourceDirectory: URL? = nil, acceptedPackage: EnginePackage? = nil) throws -> AsyncThrowingStream<EngineEvent, Error> {
        try Task.checkCancellation()
        let capabilities = acceptedPackage?.capabilities ?? capabilities
        let resourceRoot = resourceDirectory?.path ?? game.workingDirectory
        let hasResourceArgument = capabilities?.resourceRootArgument == "--resource-root"
        var launchGame = game
        if !hasResourceArgument { launchGame.workingDirectory = resourceRoot }
        try Self.validate(engine: engine, game: launchGame, capabilities: capabilities)
        try Self.validateModules(game: game, capabilities: capabilities)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: resourceRoot, isDirectory: &isDirectory), isDirectory.boolValue else {
            throw LauncherError("The game's resource folder is unavailable. Reconnect its drive or choose another folder.")
        }
        lock.lock()
        guard process == nil else { lock.unlock(); throw LauncherError("An AnyPS5 session is already running.") }
        let child = Process()
        let pipe = Pipe()
        child.executableURL = engine
        if acceptedPackage != nil { child.environment = EnginePackage.controlledEnvironment }
        let resourceArguments = hasResourceArgument ? ["--resource-root", resourceRoot] : []
        let moduleArguments = game.sceModulePaths.flatMap { ["--sce-module", $0] }
        let arguments = resourceArguments + (capabilities != nil ? ["--diagnostics-json"] : []) + moduleArguments + [game.executablePath]
        let workingDirectory = URL(fileURLWithPath: launchGame.workingDirectory, isDirectory: true)
        child.arguments = arguments
        child.currentDirectoryURL = workingDirectory
        child.standardOutput = pipe
        child.standardError = pipe
        child.standardInput = FileHandle.nullDevice
        process = child
        stopRequested = false
        lock.unlock()
        return AsyncThrowingStream { continuation in
            continuation.onTermination = { [weak self] _ in self?.stop(child: child) }
            DispatchQueue.global(qos: .userInitiated).async { [self] in
                var started = false
                do {
                    // Foundation ties child-exit delivery to the launching thread's run loop.
                    // Launch and wait on the same worker, keeping the UI and Swift executor free.
                    if let acceptedPackage {
                        try acceptedPackage.verifyIntegrityOnWorker(for: engine) { [self] in
                            self.lock.lock(); defer { self.lock.unlock() }
                            if self.stopRequested { throw CancellationError() }
                        }
                    }
                    // Reserve the start under the same lock stop() uses. Cancellation during
                    // preparation prevents a child, rather than starting one just to kill it.
                    lock.lock()
                    do {
                        if stopRequested { throw CancellationError() }
                        try child.run()
                        started = true
                        lock.unlock()
                    } catch { lock.unlock(); throw error }
                    // These immutable values are the invocation assigned to this child above.
                    continuation.yield(.started(pid: child.processIdentifier, executable: engine.path,
                                                arguments: arguments, workingDirectory: workingDirectory.path))
                    // Closing the parent's writer makes EOF observable after child shutdown.
                    try? pipe.fileHandleForWriting.close()
                    var buffer = [UInt8](repeating: 0, count: 4096)
                    while true {
                        // FileHandle.read(upToCount:) can wait to fill a pipe buffer. POSIX read
                        // returns available bytes immediately, so game diagnostics stream live.
                        let count = buffer.withUnsafeMutableBytes { bytes in
                            Darwin.read(pipe.fileHandleForReading.fileDescriptor, bytes.baseAddress, bytes.count)
                        }
                        if count == 0 { break }
                        if count < 0 {
                            if errno == EINTR { continue }
                            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
                        }
                        continuation.yield(.output(Data(buffer.prefix(count))))
                    }
                    child.waitUntilExit()
                    lock.lock()
                    process = nil
                    lock.unlock()
                    continuation.yield(.exited(child.terminationStatus))
                    continuation.finish()
                } catch {
                    stop()
                    if started { child.waitUntilExit() }
                    lock.lock()
                    process = nil
                    lock.unlock()
                    continuation.finish(throwing: error)
                }
                try? pipe.fileHandleForWriting.close()
                try? pipe.fileHandleForReading.close()
            }
        }
    }

    private func stop(child: Process) {
        lock.lock()
        defer { lock.unlock() }
        guard process === child else { return }
        stopRequested = true
        if child.isRunning { child.terminate() }
    }

    public func stop() {
        lock.lock()
        defer { lock.unlock() }
        stopRequested = true
        if let process, process.isRunning { process.terminate() }
    }

    // Launch-only checks: the CLI's metadata inspection does not accept modules.
    private static func validateModules(game: LocalGame, capabilities: EngineCapabilities?) throws {
        guard !game.sceModulePaths.isEmpty else { return }
        guard capabilities?.sceModuleArgument == "--sce-module",
              capabilities?.supportedFormats.contains("sce_elf64_x86_64") == true else {
            throw LauncherError("This engine does not advertise loading supplied SCE game libraries. Choose a supported engine or remove the attached libraries.")
        }
        let input = try FileHandle(forReadingFrom: URL(fileURLWithPath: game.executablePath))
        defer { try? input.close() }
        let header = try input.read(upToCount: 64) ?? Data()
        let magic = Array(header.prefix(4))
        let selfContainer = magic == [0x4f, 0x15, 0x3d, 0x1d] || magic == [0x54, 0x14, 0xf5, 0xee]
        let type = header.count >= 18 ? UInt16(header[16]) | (UInt16(header[17]) << 8) : 0
        let sceELF = header.count >= 64 && magic == [0x7f, 0x45, 0x4c, 0x46] &&
            (type == 0xfe10 || type == 0xfe18 || (type == 3 && (header[7] == 9 || header[8] == 2)))
        guard selfContainer || sceELF else { throw LauncherError("Attached game libraries require an SCE executable.") }
        for path in game.sceModulePaths {
            let attributes = try? FileManager.default.attributesOfItem(atPath: path)
            guard (path as NSString).isAbsolutePath,
                  attributes?[.type] as? FileAttributeType == .typeRegular,
                  FileManager.default.isReadableFile(atPath: path),
                  let module = FileHandle(forReadingAtPath: path) else {
                throw LauncherError("The game library is unavailable or is not a readable regular file: \(path)")
            }
            try module.close()
        }
    }
}
