import Foundation

public enum EngineEvent: Sendable {
    case output(Data)
    case exited(Int32)
}

/// AnyPS5's CLI receives a guest path as one argument and resolves resources from the chosen directory.
public final class EngineRunner: @unchecked Sendable {
    private let lock = NSLock()
    private var process: Process?

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
        guard header.count >= 64, Array(header.prefix(7)) == [0x7f, 0x45, 0x4c, 0x46, 2, 1, 1],
              header[18] == 62, header[19] == 0 else {
            throw LauncherError("AnyPS5 needs a clean x86-64 ELF executable. FFPFSC, exFAT, PKG, archives and encrypted SELF files cannot be launched directly.")
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

    public func run(engine: URL, game: LocalGame, capabilities: EngineCapabilities? = nil) throws -> AsyncThrowingStream<EngineEvent, Error> {
        try Self.validate(engine: engine, game: game, capabilities: capabilities)
        lock.lock()
        guard process == nil else { lock.unlock(); throw LauncherError("An AnyPS5 session is already running.") }
        let child = Process()
        let pipe = Pipe()
        child.executableURL = engine
        child.arguments = (capabilities != nil ? ["--diagnostics-json"] : []) + [game.executablePath]
        child.currentDirectoryURL = URL(fileURLWithPath: game.workingDirectory, isDirectory: true)
        child.standardOutput = pipe
        child.standardError = pipe
        child.standardInput = FileHandle.nullDevice
        process = child
        do { try child.run() }
        catch { process = nil; lock.unlock(); throw error }
        lock.unlock()
        // Closing the parent's writer makes EOF observable after the child closes its copy.
        try? pipe.fileHandleForWriting.close()
        return AsyncThrowingStream { continuation in
            DispatchQueue.global(qos: .userInitiated).async { [self] in
                do {
                    while let bytes = try pipe.fileHandleForReading.read(upToCount: 4096), !bytes.isEmpty {
                        continuation.yield(.output(bytes))
                    }
                    child.waitUntilExit()
                    lock.lock()
                    process = nil
                    lock.unlock()
                    continuation.yield(.exited(child.terminationStatus))
                    continuation.finish()
                } catch {
                    stop()
                    child.waitUntilExit()
                    lock.lock()
                    process = nil
                    lock.unlock()
                    continuation.finish(throwing: error)
                }
                try? pipe.fileHandleForReading.close()
            }
        }
    }

    public func stop() {
        lock.lock()
        defer { lock.unlock() }
        if let process, process.isRunning { process.terminate() }
    }
}
