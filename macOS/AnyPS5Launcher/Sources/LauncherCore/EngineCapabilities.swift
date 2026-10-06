import Foundation

public struct EngineCapabilitiesUnavailable: LocalizedError, Sendable {
    public let exitCode: Int32
    public var errorDescription: String? { "Engine capability probe failed (exit \(exitCode))." }
}

public struct EngineCapabilities: Decodable, Sendable {
    public let schemaVersion: Int
    public let hostArchitecture: String
    public let guestArchitecture: String
    public let backend: String
    public let supportedFormats: [String]
    public let supportedContainers: [String]?
    public let unsupportedInstructionFamilies: [String]?
    public let resourceRootArgument: String?
    public let sceModuleArgument: String?
    public let runtimeABI: String
    public let runtimeABIs: [String]?
    public let sceConstraints: [String]?
    public let ps5GameRuntimeReady: Bool

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version", hostArchitecture = "host_architecture"
        case guestArchitecture = "guest_architecture", supportedFormats = "supported_formats"
        case supportedContainers = "supported_containers", unsupportedInstructionFamilies = "unsupported_instruction_families"
        case resourceRootArgument = "resource_root_argument"
        case sceModuleArgument = "sce_module_argument"
        case runtimeABI = "runtime_abi", ps5GameRuntimeReady = "ps5_game_runtime_ready"
        case runtimeABIs = "runtime_abis", sceConstraints = "sce_constraints"
        case backend
    }

    public static func decode(_ data: Data) throws -> Self {
        let result = try JSONDecoder().decode(Self.self, from: data)
        guard result.schemaVersion == 1, result.hostArchitecture == "arm64", result.guestArchitecture == "x86_64" else {
            throw LauncherError("The engine does not advertise the supported native ARM64/x86-64 protocol.")
        }
        return result
    }

    public static func probe(_ engine: URL) async throws -> Self {
        try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let process = Process()
                let pipe = Pipe()
                process.executableURL = engine
                process.arguments = ["--capabilities-json"]
                process.standardOutput = pipe
                process.standardError = FileHandle.nullDevice
                process.standardInput = FileHandle.nullDevice
                let timeout = DispatchWorkItem { if process.isRunning { process.terminate() } }
                do {
                    try process.run()
                    try? pipe.fileHandleForWriting.close()
                    DispatchQueue.global().asyncAfter(deadline: .now() + 5, execute: timeout)
                    var data = Data()
                    while let bytes = try pipe.fileHandleForReading.read(upToCount: 4096), !bytes.isEmpty {
                        data.append(bytes)
                        if data.count > 65_536 { process.terminate(); throw LauncherError("The engine returned excessive capability output.") }
                    }
                    process.waitUntilExit()
                    timeout.cancel()
                    guard process.terminationStatus == 0 else { throw EngineCapabilitiesUnavailable(exitCode: process.terminationStatus) }
                    continuation.resume(returning: try decode(data))
                } catch { timeout.cancel(); continuation.resume(throwing: error) }
                try? pipe.fileHandleForReading.close()
            }
        }
    }
}
