//
//  DriverInstaller.swift
//  ScreamMusic
//
//  Installs, updates and removes ScreamMusic.driver (carried in the app's Resources) in
//  /Library/Audio/Plug-Ins/HAL. One administrator prompt; coreaudiod restarts, so all audio
//  stops for a moment. Approach from Nativerate's VirtualOutputPlugin.swift (GPL-3.0).
//

import AppKit
import CoreAudio
import Foundation

@MainActor
final class DriverInstaller: ObservableObject {

    static let installPath = "/Library/Audio/Plug-Ins/HAL/ScreamMusic.driver"

    enum State: Equatable {
        case notInstalled
        case installed(version: String)
        case outdated(installed: String, bundled: String)
    }

    @Published private(set) var state: State = .notInstalled
    @Published private(set) var busy = false
    @Published private(set) var lastError: String?

    init() { refresh() }

    var bundledURL: URL? { Bundle.main.url(forResource: "ScreamMusic", withExtension: "driver") }

    nonisolated static func version(of url: URL) -> (short: String, build: Int)? {
        guard let info = NSDictionary(contentsOf: url.appendingPathComponent("Contents/Info.plist")) else { return nil }
        return (info["CFBundleShortVersionString"] as? String ?? "?", Int(info["CFBundleVersion"] as? String ?? "") ?? 0)
    }

    func refresh() {
        let installed = Self.version(of: URL(fileURLWithPath: Self.installPath))
        let bundled = bundledURL.flatMap { Self.version(of: $0) }
        if let i = installed {
            state = (bundled.map { $0.build > i.build } ?? false) ? .outdated(installed: i.short, bundled: bundled!.short) : .installed(version: i.short)
        } else {
            state = .notInstalled
        }
    }

    func install() {
        guard let src = bundledURL else {
            lastError = "This build has no ScreamMusic.driver in its Resources."
            return
        }
        let dst = Self.installPath
        // coreaudiod loads plug-ins as _coreaudiod: everything must be world-readable
        run("rm -rf \(q(dst)) && /usr/bin/ditto \(q(src.path)) \(q(dst)) && /usr/sbin/chown -R root:wheel \(q(dst)) && /bin/chmod -R a+rX \(q(dst)) && /usr/bin/killall coreaudiod",
            prompt: "ScreamMusic wants to install its audio driver. Audio restarts for a moment.")
    }

    func remove() {
        run("rm -rf \(q(Self.installPath)) && /usr/bin/killall coreaudiod",
            prompt: "ScreamMusic wants to remove its audio driver. Audio restarts for a moment.")
    }

    private func run(_ shell: String, prompt: String) {
        busy = true
        lastError = nil
        let source = "do shell script \"\(Self.escape(shell))\" with administrator privileges with prompt \"\(Self.escape(prompt))\""
        Task.detached {
            var err: NSDictionary?
            let ok = NSAppleScript(source: source)?.executeAndReturnError(&err) != nil
            if ok {
                // coreaudiod restarts under launchd; wait until it lists devices again
                try? await Task.sleep(for: .seconds(1))
                let end = Date().addingTimeInterval(10)
                while Date() < end, !Self.halResponds() { try? await Task.sleep(for: .milliseconds(200)) }
            }
            let cancelled = (err?[NSAppleScript.errorNumber] as? Int) == -128
            let message = err.map { ($0[NSAppleScript.errorMessage] as? String) ?? "\($0)" }
            await MainActor.run {
                self.busy = false
                if !ok, !cancelled { self.lastError = message }
                self.refresh()
            }
        }
    }

    nonisolated private static func halResponds() -> Bool {
        var a = ScreamDevice.address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        return AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size) == noErr && size > 0
    }

    /// Single-quoted for the shell.
    private func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    /// Escaped for an AppleScript string literal.
    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
    }
}
