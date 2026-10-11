// swift-tools-version:6.0
// ScreamMusic menu-bar app. build.sh wraps the executable into ScreamMusic.app and embeds
// the HAL driver (Driver/) in its Resources.
import PackageDescription

let package = Package(
    name: "ScreamMusic",
    platforms: [.macOS("26.0")],
    targets: [
        .executableTarget(
            name: "ScreamMusic",
            path: "App/Sources/ScreamMusic",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
