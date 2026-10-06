// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "MacPS",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "MacPS", targets: ["MacPS"])],
    targets: [
        .target(name: "LauncherCore"),
        .executableTarget(name: "MacPS", dependencies: ["LauncherCore"]),
        .testTarget(name: "LauncherCoreTests", dependencies: ["LauncherCore"])
    ]
)
