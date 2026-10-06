// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "AnyPS5Launcher",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "AnyPS5Launcher", targets: ["AnyPS5Launcher"])],
    targets: [
        .target(name: "LauncherCore"),
        .executableTarget(name: "AnyPS5Launcher", dependencies: ["LauncherCore"]),
        .testTarget(name: "LauncherCoreTests", dependencies: ["LauncherCore"])
    ]
)
