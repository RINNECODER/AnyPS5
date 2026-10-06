import SwiftUI

struct ContentView: View {
    @Bindable var store: LauncherStore
    var isPreview = false
    @AppStorage("MacPS.consoleInterface") private var consoleInterface = true

    var body: some View {
        if consoleInterface {
            ConsoleRootView(store: store, isPreview: isPreview)
        } else {
            LegacyContentView(store: store).disabled(isPreview)
        }
    }
}
