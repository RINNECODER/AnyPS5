import SwiftUI

struct EngineSettingsView: View {
    @Bindable var store: LauncherStore
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("AnyPS5 Engine").font(.largeTitle.bold())
                    Text("Native ARM64 execution. Metal graphics.").foregroundStyle(.secondary)
                }
                GroupBox {
                    VStack(alignment: .leading, spacing: 12) {
                        Label("Runtime executable", systemImage: "cpu").font(.headline)
                        Text(store.library.enginePath.isEmpty ? "No engine selected" : store.library.enginePath)
                            .font(.callout.monospaced()).textSelection(.enabled)
                        Button("Choose AnyPS5 Runtime…") { store.chooseEngine() }
                            .disabled(store.isProbingEngine || store.isRunning)
                        Text(store.engineProbeStatus).font(.caption).foregroundStyle(.secondary)
                        if let capabilities = store.capabilities {
                            Text("Inputs: \(capabilities.supportedFormats.joined(separator: ", "))").font(.caption.monospaced())
                            Label(capabilities.ps5GameRuntimeReady ? "Engine reports PS5 game runtime ready" : "PS5 game runtime in development",
                                  systemImage: capabilities.ps5GameRuntimeReady ? "checkmark.circle" : "hammer")
                                .font(.caption)
                        }
                        Button("Recheck Capabilities") { Task { await store.probeEngine() } }
                            .disabled(store.library.enginePath.isEmpty || store.isProbingEngine || store.isRunning)
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(12)
                }
                GroupBox("Current support") {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("The current anyps5_cpu_run checkpoint runs static x86-64 ELF programs on Apple Silicon. PS5 game loading, system-library calls and game graphics are still in development.")
                        Text("Orbit's exFAT and FFPFSC releases cannot be passed directly to this engine. Attach a clean executable to test the runtime; unsupported inputs and calls are reported in the console.")
                        Text("A successful process exit records that session's result. It does not establish graphics correctness or playable game compatibility.")
                            .foregroundStyle(.secondary)
                    }.font(.callout).padding(12)
                }
                GroupBox("Launch contract") {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("anyps5_cpu_run <static-x86-64.elf>").font(.callout.monospaced())
                        Text("The executable path is passed as one argument. The resource folder becomes the engine's working directory. Standard output, errors and the exit code appear in the console.")
                            .font(.callout).foregroundStyle(.secondary)
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(12)
                }
            }.padding(32).frame(maxWidth: 780, alignment: .leading)
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}
