import Foundation

enum ConsoleResources {
    /// Resolve the shipped app first; SwiftPM's generated search locations vary by toolchain.
    static let bundle: Bundle = {
        if let url = Bundle.main.resourceURL?.appendingPathComponent("AnyPS5Launcher_AnyPS5Launcher.bundle"),
           let bundle = Bundle(url: url),
           bundle.url(forResource: "ConsoleCoast", withExtension: "png") != nil {
            return bundle
        }
        return .module
    }()
}
