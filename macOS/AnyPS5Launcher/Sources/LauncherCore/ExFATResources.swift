import Foundation

/// A session owns this mount. The source image is never opened for writing.
public struct MountedExFATResources: Sendable {
    public let directory: URL
    fileprivate let device: String
    fileprivate let image: URL

    fileprivate init(directory: URL, device: String, image: URL) {
        self.directory = directory
        self.device = device
        self.image = image
    }

    public func unmount() async throws {
        // Device numbers can be reused. Never detach an unrelated image after external unmount.
        guard let current = try await ExFATResources.ownedMount(image: image, directory: directory), current.device == device else { return }
        _ = try await ExFATResources.command("/usr/bin/hdiutil", ["detach", device])
    }
}

public struct ResourceImageCleanupFailure: LocalizedError, Sendable {
    public let resources: MountedExFATResources
    public let message: String
    public var errorDescription: String? { message }
}

public enum ExFATResources {
    public static func validate(_ image: URL) throws {
        guard !["crdownload", "part", "download"].contains(image.pathExtension.lowercased()) else {
            throw LauncherError("The game image is still downloading. Select its completed exFAT file.")
        }
        let attributes = try FileManager.default.attributesOfItem(atPath: image.path)
        guard attributes[.type] as? FileAttributeType == .typeRegular else {
            throw LauncherError("Select a completed local exFAT image file.")
        }
        let handle = try FileHandle(forReadingFrom: image)
        defer { try? handle.close() }
        let header = try handle.read(upToCount: 512) ?? Data()
        guard header.count == 512, String(decoding: header[3..<11], as: UTF8.self) == "EXFAT   ",
              header[510] == 0x55, header[511] == 0xaa, (9...12).contains(header[108]) else {
            throw LauncherError("This file is not a raw exFAT volume. Choose an exFAT release or an extracted resource folder.")
        }
        let sectors = header[72..<80].enumerated().reduce(UInt64(0)) { $0 | (UInt64($1.element) << (8 * $1.offset)) }
        let (expectedSize, overflow) = sectors.multipliedReportingOverflow(by: UInt64(1) << header[108])
        let size = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
        guard !overflow, sectors > 0, expectedSize == size else {
            throw LauncherError("The exFAT image size does not match its volume header. Wait for the download to finish or check the source file.")
        }
    }

    public static func mount(_ image: URL) async throws -> MountedExFATResources {
        try validate(image)
        let existing = try await attachments()
        guard !existing.contains(where: { record in
            guard let path = record["image-path"] as? String else { return false }
            return sameFile(URL(fileURLWithPath: path), image)
        }) else {
            throw LauncherError("This resource image is already attached. Eject its existing volume before starting an AnyPS5 session.")
        }
        let parent = FileManager.default.temporaryDirectory.appendingPathComponent("AnyPS5Resources", isDirectory: true)
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        let directory = parent.appendingPathComponent(UUID().uuidString, isDirectory: true)
        var acquired: MountedExFATResources?
        do {
            _ = try await command("/usr/bin/hdiutil", ["attach", "-readonly", "-nobrowse", "-noautofsck",
                "-mountpoint", directory.path, "-imagekey", "diskimage-class=CRawDiskImage", "-plist", image.path])
            // attach can reuse an existing image even after the preflight race. Ownership requires
            // both this source and the unique mount point created for this request.
            guard let owned = try await ownedMount(image: image, directory: directory) else {
                throw LauncherError("macOS did not create a resource mount owned by this session.")
            }
            acquired = owned
            let infoData = try await command("/usr/sbin/diskutil", ["info", "-plist", owned.device])
            let info = try PropertyListSerialization.propertyList(from: infoData, format: nil) as? [String: Any]
            guard info?["WritableMedia"] as? Bool == false, info?["WritableVolume"] as? Bool == false,
                  info?["FilesystemType"] as? String == "exfat",
                  let path = info?["MountPoint"] as? String,
                  URL(fileURLWithPath: path).resolvingSymlinksInPath() == directory.resolvingSymlinksInPath() else {
                throw LauncherError("The image did not mount as the expected read-only exFAT resource volume.")
            }
            return owned
        } catch {
            var owned = acquired
            if owned == nil { owned = try? await ownedMount(image: image, directory: directory) }
            if let owned {
                do { try await owned.unmount() }
                catch let cleanupError {
                    throw ResourceImageCleanupFailure(resources: owned, message: "\(error.localizedDescription) Resource cleanup also failed: \(cleanupError.localizedDescription)")
                }
            }
            throw error
        }
    }

    private static func sameFile(_ first: URL, _ second: URL) -> Bool {
        if first.resolvingSymlinksInPath() == second.resolvingSymlinksInPath() { return true }
        guard let a = try? FileManager.default.attributesOfItem(atPath: first.path),
              let b = try? FileManager.default.attributesOfItem(atPath: second.path) else { return false }
        return a[.systemFileNumber] as? NSNumber == b[.systemFileNumber] as? NSNumber &&
               a[.systemNumber] as? NSNumber == b[.systemNumber] as? NSNumber
    }

    private static func attachments() async throws -> [[String: Any]] {
        let data = try await command("/usr/bin/hdiutil", ["info", "-plist"])
        let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        guard let images = plist?["images"] as? [[String: Any]] else { throw LauncherError("Cannot read macOS image ownership information.") }
        return images
    }

    fileprivate static func ownedMount(image: URL, directory: URL) async throws -> MountedExFATResources? {
        for record in try await attachments() {
            guard let path = record["image-path"] as? String, sameFile(URL(fileURLWithPath: path), image) else { continue }
            for entity in record["system-entities"] as? [[String: Any]] ?? [] {
                guard let mountPoint = entity["mount-point"] as? String,
                      URL(fileURLWithPath: mountPoint).resolvingSymlinksInPath() == directory.resolvingSymlinksInPath(),
                      let raw = entity["dev-entry"] as? String else { continue }
                let device = raw.hasPrefix("/dev/") ? raw : "/dev/" + raw
                guard device.range(of: #"^/dev/disk[0-9]+(s[0-9]+)?$"#, options: .regularExpression) != nil else {
                    throw LauncherError("macOS returned an unexpected disk identifier.")
                }
                return MountedExFATResources(directory: directory.resolvingSymlinksInPath(), device: device, image: image)
            }
        }
        return nil
    }

    fileprivate static func command(_ executable: String, _ arguments: [String]) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let child = Process()
                let output = Pipe()
                let diagnosticURL = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
                do {
                    // A file avoids deadlocking two output pipes while macOS checks the volume.
                    guard FileManager.default.createFile(atPath: diagnosticURL.path, contents: nil) else {
                        throw LauncherError("Cannot create a temporary image diagnostic file.")
                    }
                    let diagnostics = try FileHandle(forWritingTo: diagnosticURL)
                    defer { try? diagnostics.close(); try? FileManager.default.removeItem(at: diagnosticURL) }
                    child.executableURL = URL(fileURLWithPath: executable)
                    child.arguments = arguments
                    child.standardInput = FileHandle.nullDevice
                    child.standardOutput = output
                    child.standardError = diagnostics
                    try child.run()
                    try? output.fileHandleForWriting.close()
                    let data = output.fileHandleForReading.readDataToEndOfFile()
                    child.waitUntilExit()
                    guard child.terminationStatus == 0 else {
                        let errorData = try Data(contentsOf: diagnosticURL)
                        throw LauncherError("macOS image operation failed (exit \(child.terminationStatus)). \(String(decoding: errorData.prefix(16_384), as: UTF8.self))")
                    }
                    continuation.resume(returning: data)
                } catch { continuation.resume(throwing: error) }
                try? output.fileHandleForReading.close()
                try? output.fileHandleForWriting.close()
            }
        }
    }
}
