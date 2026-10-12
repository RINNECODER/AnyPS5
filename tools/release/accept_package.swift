// Runs MacPS's production EnginePackage.accept on a package directory, pinned to its
// manifest SHA-256, and prints the accepted identity as JSON (tools/release/macps_accept.py).
import Foundation
import LauncherCore

@main struct AcceptPackage {
    static func main() async {
        do {
            guard CommandLine.arguments.count == 3 else { throw LauncherError("Supply the package path and exact manifest SHA-256.") }
            let expected = CommandLine.arguments[2]
            guard expected.utf8.count == 64, expected.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
                throw LauncherError("The expected manifest SHA-256 must be 64 lowercase hexadecimal characters.")
            }
            let package = try await EnginePackage.accept(selectedURL: URL(fileURLWithPath: CommandLine.arguments[1]), expectedManifestSHA256: expected)
            let record: [String: Any] = ["package_root": package.rootURL.path, "engine_executable": package.executableURL.path,
                "manifest_sha256": package.manifestSHA256, "source_commit": package.sourceCommit,
                "backend": package.capabilities.backend, "ps5_game_runtime_ready": package.capabilities.ps5GameRuntimeReady,
                "architecture": package.architecture.rawValue, "surface": "production EnginePackage.accept"]
            let data = try JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys])
            print(String(decoding: data, as: UTF8.self))
        } catch { fputs("Package acceptance stopped: \(error.localizedDescription)\n", stderr); exit(2) }
    }
}
