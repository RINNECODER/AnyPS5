import AppKit
import Foundation
import LauncherCore
import Observation

@MainActor @Observable
final class LauncherStore {
    var selectedID: String?
    var games: [CatalogueGame] = []
    var library = LauncherLibrary()
    var isRefreshing = false
    var snapshot: CatalogueSnapshot?
    var catalogueError: String?
    var error: String?
    var isRunning = false
    var runningTitle: String?
    var sessionStatus = "No session yet"
    var console = "Add a game to your library to start a session."
    var showConsole = false
    var capabilities: EngineCapabilities?
    var engineProbeStatus = "Choose an engine to read its capabilities."
    var isProbingEngine = false
    var isInspectingGame = false
    private(set) var isCleaningResources = false
    var inspectedGame: LocalGame?
    var inspectionText: String?
    private var engineCompatibilityError: String?
    private var acceptedPackage: EnginePackage?

    private let persistence: LibraryPersistence
    private let client: CatalogueClient
    private let runner = EngineRunner()
    private var logBytes = Data()
    private var canSave = true
    private var sessionCancelled = false
    private var mountedResources: MountedExFATResources?
    var hasMountedResources: Bool { mountedResources != nil }

    init(supportDirectory: URL? = nil) {
        let support = supportDirectory ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("MacPS", isDirectory: true)
        persistence = LibraryPersistence(url: support.appendingPathComponent("library.json"))
        client = CatalogueClient(cacheURL: support.appendingPathComponent("orbit-catalogue-v2.json"))
        do {
            if supportDirectory == nil {
                _ = try MacPSStorage.prepare(in: support.deletingLastPathComponent())
            }
            library = try persistence.load()
        }
        catch {
            canSave = false
            self.error = "Your library could not be read. It has been preserved at \(support.path)/library.json. \(error.localizedDescription)"
        }
    }

    var selectedLocal: LocalGame? { library.games.first { $0.id == selectedID } }

    func refresh() async {
        guard !isRefreshing else { return }
        isRefreshing = true
        defer { isRefreshing = false }
        do {
            let snapshot = try await client.refresh()
            self.snapshot = snapshot
            games = snapshot.catalogue.games
            catalogueError = nil
        } catch { catalogueError = error.localizedDescription }
    }

    func attach(to game: CatalogueGame? = nil, replacing local: LocalGame? = nil) {
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        let panel = NSOpenPanel()
        panel.title = "Choose a local game executable"
        panel.message = "Select an x86-64 ELF or a supported plaintext SELF eboot.bin. Disk images cannot run directly."
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let existing = library.games.first { $0.executablePath == url.path }
        let entry = LocalGame(id: game?.id ?? local?.id ?? existing?.id ?? "local-\(UUID().uuidString)",
                              title: game?.title ?? local?.title ?? url.deletingPathExtension().lastPathComponent,
                              executablePath: url.path, workingDirectory: url.deletingLastPathComponent().path,
                              resourceImagePath: local?.resourceImagePath ?? existing?.resourceImagePath,
                              sceModulePaths: local?.sceModulePaths ?? existing?.sceModulePaths ?? [])
        library.attach(entry)
        selectedID = entry.id
        save()
    }

    func chooseEngine() {
        guard !isRunning, !isProbingEngine, !isInspectingGame else { return }
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        let panel = NSOpenPanel()
        panel.title = "Choose the AnyPS5 engine"
        panel.message = "Choose an AnyPS5 engine package folder, its manifest, or a runtime executable. Packages are checked before selection is saved."
        panel.canChooseDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await selectEngine(url) }
    }

    private func isPackageSelection(_ url: URL) -> Bool {
        if url.hasDirectoryPath || url.lastPathComponent == "manifest.json" { return true }
        guard url.lastPathComponent == "anyps5_cpu_run", url.deletingLastPathComponent().lastPathComponent == "bin" else { return false }
        return FileManager.default.fileExists(atPath: url.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("manifest.json").path)
    }

    private func selectEngine(_ url: URL) async {
        guard !isRunning, !isProbingEngine, !isInspectingGame, canSave else { return }
        isProbingEngine = true
        defer { isProbingEngine = false }
        do {
            let package: EnginePackage?
            let result: EngineCapabilities?
            let executable: URL
            if isPackageSelection(url) {
                engineProbeStatus = "Checking engine package and compiled execution fixtures…"
                let verified = try await EnginePackage.accept(selectedURL: url)
                package = verified; result = verified.capabilities; executable = verified.executableURL
            } else {
                guard FileManager.default.isExecutableFile(atPath: url.path) else {
                    throw LauncherError("The selected file is not executable. Choose an AnyPS5 runtime or package.")
                }
                package = nil; executable = url
                do { result = try await EngineCapabilities.probe(url) }
                catch is EngineCapabilitiesUnavailable { result = nil }
            }
            var updated = library
            updated.enginePath = executable.path
            updated.enginePackageManifestSHA256 = package?.manifestSHA256
            try persistence.save(updated)
            library = updated
            acceptedPackage = package
            capabilities = result
            inspectedGame = nil
            inspectionText = nil
            engineCompatibilityError = nil
            updateEngineStatus()
        } catch {
            self.error = "Engine selection failed: \(error.localizedDescription)"
            updateEngineStatus()
        }
    }

    private func updateEngineStatus() {
        if let package = acceptedPackage {
            engineProbeStatus = "Package accepted · \(package.capabilities.backend) · source \(package.sourceCommit.prefix(8)) · TCG \(package.engineCommit.prefix(8))"
        } else if let capabilities {
            engineProbeStatus = "\(capabilities.backend) · \(capabilities.hostArchitecture) → \(capabilities.guestArchitecture) · \((capabilities.runtimeABIs ?? [capabilities.runtimeABI]).joined(separator: ", "))"
        } else {
            engineProbeStatus = library.enginePath.isEmpty ? "Choose an engine to read its capabilities." : "Capabilities unavailable. Using the legacy static ELF checkpoint contract."
        }
    }

    func probeEngine() async {
        guard !library.enginePath.isEmpty, !isProbingEngine else { return }
        isProbingEngine = true
        let path = library.enginePath
        defer { isProbingEngine = false }
        do {
            let url = URL(fileURLWithPath: path)
            let result: EngineCapabilities
            if library.enginePackageManifestSHA256 != nil || isPackageSelection(url) {
                let package = try await EnginePackage.accept(selectedURL: url, expectedManifestSHA256: library.enginePackageManifestSHA256)
                if let expected = library.enginePackageManifestSHA256, package.manifestSHA256 != expected {
                    throw LauncherError("The selected engine package changed since it was saved. Choose the updated package explicitly to accept it.")
                }
                guard library.enginePath == path else { return }
                acceptedPackage = package
                result = package.capabilities
                if library.enginePackageManifestSHA256 == nil {
                    var updated = library
                    updated.enginePackageManifestSHA256 = package.manifestSHA256
                    try persistence.save(updated)
                    library = updated
                }
            } else {
                result = try await EngineCapabilities.probe(url)
                acceptedPackage = nil
            }
            guard library.enginePath == path else { return }
            capabilities = result
            engineCompatibilityError = nil
            updateEngineStatus()
        } catch {
            guard library.enginePath == path else { return }
            capabilities = nil
            acceptedPackage = nil
            if error is EngineCapabilitiesUnavailable, library.enginePackageManifestSHA256 == nil,
               !isPackageSelection(URL(fileURLWithPath: path)) {
                engineCompatibilityError = nil
                engineProbeStatus = "Capabilities unavailable. Using the legacy static ELF checkpoint contract. \(error.localizedDescription)"
            } else {
                engineCompatibilityError = error.localizedDescription
                engineProbeStatus = "Engine capability check failed: \(error.localizedDescription)"
            }
        }
    }

    func chooseResources(for game: LocalGame) {
        guard canSave else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose the game resource folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.directoryURL = URL(fileURLWithPath: game.workingDirectory)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var updated = game
        updated.workingDirectory = url.path
        updated.resourceImagePath = nil
        library.attach(updated)
        save()
    }

    func chooseImageResources(for game: LocalGame) {
        guard canSave, !isRunning else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose a completed exFAT resource image"
        panel.message = "AnyPS5 mounts this image read-only during the session. Attach a separate supported ELF or plaintext SELF executable."
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try ExFATResources.validate(url)
            var updated = game
            updated.resourceImagePath = url.path
            library.attach(updated)
            save()
        } catch { self.error = error.localizedDescription }
    }

    func chooseModules(for game: LocalGame) {
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        guard !isRunning, !isInspectingGame, !isCleaningResources else { return }
        let panel = NSOpenPanel()
        panel.title = "Choose local game modules"
        panel.message = "Select local SCE ELF or supported plaintext SELF libraries required by this game."
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = true
        panel.directoryURL = URL(fileURLWithPath: game.executablePath).deletingLastPathComponent()
        guard panel.runModal() == .OK,
              var updated = library.games.first(where: { $0.id == game.id }) else { return }
        for url in panel.urls where !updated.sceModulePaths.contains(url.path) {
            updated.sceModulePaths.append(url.path)
        }
        library.attach(updated)
        save()
    }

    func removeModule(_ path: String, from game: LocalGame) {
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        guard !isRunning, !isInspectingGame, !isCleaningResources,
              var updated = library.games.first(where: { $0.id == game.id }),
              let index = updated.sceModulePaths.firstIndex(of: path) else { return }
        updated.sceModulePaths.remove(at: index)
        library.attach(updated)
        save()
    }

    func launch(_ game: LocalGame) {
        guard !isRunning, !isInspectingGame, !isCleaningResources else { return }
        guard !isProbingEngine else { error = "Wait for the engine capability check to finish."; return }
        if let engineCompatibilityError { error = engineCompatibilityError; return }
        if (library.enginePackageManifestSHA256 != nil || isPackageSelection(URL(fileURLWithPath: library.enginePath))),
           acceptedPackage == nil {
            error = "Recheck the saved engine package before launching a game."
            return
        }
        isRunning = true
        sessionCancelled = false
        runningTitle = game.title
        sessionStatus = "Preparing \(game.title)"
        logBytes = Data()
        console = "Preparing local game resources…\n"
        showConsole = UserDefaults.standard.bool(forKey: "MacPS.showActivityAfterLaunch")
        Task {
            defer { isRunning = false; runningTitle = nil }
            do {
                try await releaseResources()
                var resourceDirectory: URL?
                if let imagePath = game.resourceImagePath {
                    let resources = try await ExFATResources.mount(URL(fileURLWithPath: imagePath))
                    mountedResources = resources
                    resourceDirectory = resources.directory
                }
                if sessionCancelled {
                    sessionStatus = "Session cancelled"
                    console += "Session cancelled before guest execution.\n"
                } else {
                    let stream = try runner.run(engine: URL(fileURLWithPath: library.enginePath), game: game,
                                                capabilities: capabilities, resourceDirectory: resourceDirectory,
                                                acceptedPackage: acceptedPackage)
                    console = "Engine: \(library.enginePath)\nGuest: \(game.executablePath)\nResources: \(resourceDirectory?.path ?? game.workingDirectory)\n\n"
                    let prefix = console
                    for try await event in stream {
                        switch event {
                        case .started:
                            if !sessionCancelled { sessionStatus = "Running \(game.title)" }
                        case .output(let bytes):
                            logBytes.append(bytes)
                            if logBytes.count > 524_288 { logBytes.removeFirst(logBytes.count - 524_288) }
                            console = prefix + String(decoding: logBytes, as: UTF8.self)
                        case .exited(let code):
                            sessionStatus = "\(game.title) exited with code \(code)"
                            console += "\n[Engine exited with code \(code)]\n"
                        }
                    }
                }
            } catch is CancellationError {
                sessionStatus = "Session cancelled"
                console += "\nSession cancelled before guest execution.\n"
            } catch {
                if let failure = error as? ResourceImageCleanupFailure { mountedResources = failure.resources }
                sessionStatus = "Engine session failed"
                console += "\n\(error.localizedDescription)\n"
                self.error = error.localizedDescription
            }
            do { try await releaseResources() }
            catch {
                console += "\nResource image could not be detached: \(error.localizedDescription)\n"
                self.error = "Resource cleanup failed. Close files using the mounted volume, then retry cleanup. \(error.localizedDescription)"
            }
        }
    }

    func inspect(_ game: LocalGame) {
        guard !isInspectingGame, !isRunning, !isProbingEngine else { return }
        guard let capabilities else { error = "Recheck engine capabilities before inspecting a game."; return }
        isInspectingGame = true
        inspectedGame = game
        inspectionText = "Reading executable metadata…"
        let enginePath = library.enginePath
        Task {
            defer { isInspectingGame = false }
            do {
                let result = try await EngineInspection.inspect(engine: URL(fileURLWithPath: enginePath), game: game,
                                                                capabilities: capabilities, acceptedPackage: acceptedPackage)
                guard library.enginePath == enginePath else { return }
                inspectionText = result.summary
            } catch {
                guard library.enginePath == enginePath else { return }
                inspectionText = error.localizedDescription
            }
        }
    }

    func stop() { sessionCancelled = true; runner.stop(); sessionStatus = "Stopping \(runningTitle ?? "engine")…" }

    func stopAndWait() async throws {
        stop()
        while isRunning || isCleaningResources { try? await Task.sleep(for: .milliseconds(100)) }
        isCleaningResources = true
        defer { isCleaningResources = false }
        try await releaseResources()
    }

    func retryResourceCleanup() async {
        guard !isRunning, !isCleaningResources else { return }
        isCleaningResources = true
        defer { isCleaningResources = false }
        do { try await releaseResources() }
        catch { self.error = "Resource cleanup failed: \(error.localizedDescription)" }
    }

    private func releaseResources() async throws {
        guard let resources = mountedResources else { return }
        try await resources.unmount()
        if mountedResources?.directory == resources.directory { mountedResources = nil }
    }

    func removeFromLibrary(_ game: LocalGame) {
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        library.games.removeAll { $0.id == game.id }
        save()
    }

    private func save() {
        guard canSave else { return }
        do { try persistence.save(library) }
        catch { self.error = "Changes could not be saved: \(error.localizedDescription)" }
    }
}
