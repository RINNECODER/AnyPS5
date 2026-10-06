import SwiftUI

struct AppSettingsView: View {
    @Bindable var store: LauncherStore
    @AppStorage("MacPS.showActivityAfterLaunch") private var showActivityAfterLaunch = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                Text("MacPS").font(.largeTitle.bold())
                GroupBox("Library") {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("Your library contains the local games you add. The catalogue helps you find their artwork and release information.")
                        Text("Adding a game does not establish compatibility. Game runtime support is still in development.")
                            .foregroundStyle(.secondary)
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(12)
                }
                GroupBox("Activity") {
                    Toggle("Show technical activity when a game starts", isOn: $showActivityAfterLaunch)
                        .padding(12)
                }
                DisclosureGroup("Advanced runtime settings") {
                    EngineSettingsView(store: store)
                }
                Text("The AnyPS5 runtime powers execution. MacPS manages your library and app interface.")
                    .font(.caption).foregroundStyle(.secondary)
            }.padding(28)
        }
    }
}
