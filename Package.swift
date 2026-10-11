// swift-tools-version:6.0
// ScreamMusic menu-bar app. build.sh wraps the executable into ScreamMusic.app and embeds
// the HAL driver (Driver/) in its Resources.
import PackageDescription

let package = Package(
    name: "ScreamMusic",
    platforms: [.macOS("26.0")],
    targets: [
        .target(
            name: "ScreamMusicKit",
            path: "App/Sources/Kit"
        ),
        .executableTarget(
            name: "ScreamMusic",
            dependencies: ["ScreamMusicKit"],
            path: "App/Sources/ScreamMusic",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "ScreamMusicKitTests",
            dependencies: ["ScreamMusicKit"],
            path: "Tests/RateLibrary"
        ),
    ]
)
