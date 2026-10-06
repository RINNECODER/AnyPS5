import AppKit
import Foundation
import LauncherCore
import Observation

@MainActor @Observable
final class LauncherStore {
    enum Section: String, CaseIterable, Identifiable {
        case catalogue = "Orbit Catalogue", library = "My Library", engine = "Engine"
        var id: String { rawValue }
        var icon: String {
            switch self { case .catalogue: "square.grid.2x2"; case .library: "gamecontroller"; case .engine: "cpu" }
        }
    }

    var section = Section.catalogue
    var search = ""
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
    var console = "Select a local executable and configure the AnyPS5 engine to start a session."
    var showConsole = false
    var capabilities: EngineCapabilities?
    var engineProbeStatus = "Choose an engine to read its capabilities."
    var isProbingEngine = false
    var isInspectingGame = false
    var inspectedGame: LocalGame?
    var inspectionText: String?
    private var engineCompatibilityError: String?

    private let persistence: LibraryPersistence
    private let client: CatalogueClient
    private let runner = EngineRunner()
    private var logBytes = Data()
    private var canSave = true

    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("AnyPS5Launcher", isDirectory: true)
        persistence = LibraryPersistence(url: support.appendingPathComponent("library.json"))
        client = CatalogueClient(cacheURL: support.appendingPathComponent("orbit-catalogue-v2.json"))
        do { library = try persistence.load() }
        catch {
            canSave = false
            self.error = "Your library could not be read. It has been preserved at \(support.path)/library.json. \(error.localizedDescription)"
        }
    }

    var visibleGames: [CatalogueGame] {
        games.filter { search.isEmpty || $0.title.localizedCaseInsensitiveContains(search) || $0.titleIDs.localizedCaseInsensitiveContains(search) }
    }
    var visibleLocalGames: [LocalGame] {
        library.games.filter { search.isEmpty || $0.title.localizedCaseInsensitiveContains(search) }
            .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }
    var selectedGame: CatalogueGame? { games.first { $0.id == selectedID } }
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
        panel.message = "Select a clean x86-64 ELF or eboot.bin. Console containers cannot run directly."
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let existing = library.games.first { $0.executablePath == url.path }
        let entry = LocalGame(id: game?.id ?? local?.id ?? existing?.id ?? "local-\(UUID().uuidString)",
                              title: game?.title ?? local?.title ?? url.deletingPathExtension().lastPathComponent,
                              executablePath: url.path, workingDirectory: url.deletingLastPathComponent().path)
        library.attach(entry)
        selectedID = entry.id
        if game == nil { section = .library }
        save()
    }

    func chooseEngine() {
        guard !isRunning, !isProbingEngine, !isInspectingGame else { return }
        guard canSave else { error = "The saved library needs to be repaired before it can be changed."; return }
        let panel = NSOpenPanel()
        panel.title = "Choose the AnyPS5 engine"
        panel.message = "Choose a built AnyPS5 CLI runtime. The current checkpoint is anyps5_cpu_run."
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard FileManager.default.isExecutableFile(atPath: url.path) else {
            error = "The selected file is not executable. Choose a built AnyPS5 runtime binary."
            return
        }
        library.enginePath = url.path
        capabilities = nil
        inspectedGame = nil
        inspectionText = nil
        engineCompatibilityError = nil
        save()
        Task { await probeEngine() }
    }

    func probeEngine() async {
        guard !library.enginePath.isEmpty, !isProbingEngine else { return }
        isProbingEngine = true
        let path = library.enginePath
        defer { isProbingEngine = false }
        do {
            let result = try await EngineCapabilities.probe(URL(fileURLWithPath: path))
            guard library.enginePath == path else { return }
            capabilities = result
            engineCompatibilityError = nil
            engineProbeStatus = "\(result.backend) · \(result.hostArchitecture) → \(result.guestArchitecture) · \((result.runtimeABIs ?? [result.runtimeABI]).joined(separator: ", "))"
        } catch {
            guard library.enginePath == path else { return }
            capabilities = nil
            if error is EngineCapabilitiesUnavailable {
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
        library.attach(updated)
        save()
    }

    func launch(_ game: LocalGame) {
        guard !isRunning, !isInspectingGame else { return }
        do {
            guard !isProbingEngine else { throw LauncherError("Wait for the engine capability check to finish.") }
            if let engineCompatibilityError { throw LauncherError(engineCompatibilityError) }
            let stream = try runner.run(engine: URL(fileURLWithPath: library.enginePath), game: game, capabilities: capabilities)
            isRunning = true
            runningTitle = game.title
            sessionStatus = "Running \(game.title)"
            logBytes = Data()
            console = "Engine: \(library.enginePath)\nGuest: \(game.executablePath)\nResources: \(game.workingDirectory)\n\n"
            let prefix = console
            showConsole = true
            Task {
                defer { isRunning = false; runningTitle = nil }
                do {
                    for try await event in stream {
                        switch event {
                        case .output(let bytes):
                            logBytes.append(bytes)
                            if logBytes.count > 524_288 { logBytes.removeFirst(logBytes.count - 524_288) }
                            console = prefix + String(decoding: logBytes, as: UTF8.self)
                        case .exited(let code):
                            sessionStatus = "\(game.title) exited with code \(code)"
                            console += "\n[Engine exited with code \(code)]\n"
                        }
                    }
                } catch {
                    sessionStatus = "Engine session failed"
                    console += "\n\(error.localizedDescription)\n"
                }
            }
        } catch { self.error = error.localizedDescription; showConsole = true }
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
                let result = try await EngineInspection.inspect(engine: URL(fileURLWithPath: enginePath), game: game, capabilities: capabilities)
                guard library.enginePath == enginePath else { return }
                inspectionText = result.summary
            } catch {
                guard library.enginePath == enginePath else { return }
                inspectionText = error.localizedDescription
            }
        }
    }

    func stop() { runner.stop(); sessionStatus = "Stopping \(runningTitle ?? "engine")…" }

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
