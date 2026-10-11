//
//  ScreamMusicApp.swift
//  ScreamMusic
//
//  Menu-bar app: installs the embedded ScreamMusic.driver, configures where it sends Scream,
//  runs Apple Music mode and shows what the driver is doing (DESIGN.md §6).
//

import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel()

    func applicationWillTerminate(_ notification: Notification) {
        model.musicMode.quit()
    }
}

@main
struct ScreamMusicApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        // A regular window too: shown at launch and when the app is opened again (double-click),
        // so the app is reachable even when macOS hides its menu-bar item.
        Window("ScreamMusic", id: "main") {
            PanelView(model: delegate.model, installer: delegate.model.installer, music: delegate.model.musicMode)
                .onAppear { NSApp.activate() }   // an LSUIElement app isn't brought forward by itself
        }
        .windowResizability(.contentSize)

        MenuBarExtra("ScreamMusic", systemImage: "hifispeaker") {
            PanelView(model: delegate.model, installer: delegate.model.installer, music: delegate.model.musicMode)
        }
        .menuBarExtraStyle(.window)
    }
}

enum AppVersion {
    static var string: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
    }
}

func rateText(_ r: Double) -> String {
    r.truncatingRemainder(dividingBy: 1000) == 0 ? "\(Int(r / 1000)) kHz" : String(format: "%.1f kHz", r / 1000)
}

struct PanelView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var installer: DriverInstaller
    @ObservedObject var music: MusicMode
    @State private var advanced = false
    @State private var confirmUninstall = false
    @State private var uninstallNote: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("ScreamMusic").font(.headline)
                Spacer()
                Text(AppVersion.string).foregroundStyle(.secondary)
            }
            Divider()
            if model.device == nil {
                driverMissing
            } else {
                statusSection
                Divider()
                musicSection
                Divider()
                destinationSection
                Divider()
                DisclosureGroup("Advanced", isExpanded: $advanced) { advancedSection }
            }
            Divider()
            driverSection
            HStack {
                Spacer()
                Button("Quit ScreamMusic") { NSApplication.shared.terminate(nil) }
                    .keyboardShortcut("q")
            }
        }
        .padding(14)
        .frame(width: 380)
        .alert("Uninstall ScreamMusic?", isPresented: $confirmUninstall) {
            Button("Uninstall", role: .destructive) { uninstall() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Turns off Apple Music mode and restores the outputs, removes the driver (administrator password; audio restarts for a moment), deletes this app's settings and logs, then deletes and quits the app.")
        }
        .alert("ScreamMusic", isPresented: Binding(get: { uninstallNote != nil }, set: { if !$0 { uninstallNote = nil } })) {
            Button("OK") { NSApplication.shared.terminate(nil) }
        } message: {
            Text(uninstallNote ?? "")
        }
    }

    private func uninstall() {
        model.uninstall { deleted in
            if deleted {
                NSApplication.shared.terminate(nil)
            } else {
                uninstallNote = "The driver, settings and logs are gone. The app is still at:\n\(Bundle.main.bundleURL.path)\n\nMove it to the Trash yourself."
            }
        }
    }

    private var driverMissing: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("The ScreamMusic audio driver is not loaded.")
            Text("Install it below. macOS asks for an administrator password, and all audio stops for a moment while the audio system restarts.")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var statusSection: some View {
        let s = model.status
        return VStack(alignment: .leading, spacing: 4) {
            if s.running {
                Text("Sending \(rateText(Double(s.rate))) / \(s.wireBits)-bit to \(model.destination.ip):\(String(model.destination.port))")
                    .fontWeight(.medium)
            } else {
                Text("Idle").fontWeight(.medium)
            }
            Group {
                switch model.bitPerfect {
                case .idle:
                    EmptyView()
                case .checking:
                    Text("Checking bit-perfect…").foregroundStyle(.secondary)
                case .perfect(let since):
                    let seconds = Int(Date().timeIntervalSince(since))
                    Text("Bit-perfect: on the \(s.wireBits)-bit grid, clean for \(seconds / 60):\(String(format: "%02d", seconds % 60))").foregroundStyle(.green)
                case .degraded(let offGrid):
                    Text("Not bit-perfect: \(offGrid) samples off the \(s.wireBits)-bit grid in the last second (resampled, volume or EQ)").foregroundStyle(.orange)
                }
                if let pid = model.hogOwner { Text("Exclusive (hog) by pid \(pid)") }
                if !s.clients.isEmpty {
                    Text("Playing: " + s.clients.map { $0.bundleID.isEmpty ? "pid \($0.pid)" : $0.bundleID }.joined(separator: ", "))
                        .lineLimit(2)
                }
                if model.underrunMs > 0 {
                    Text("Sender stalled ~\(model.underrunMs) ms just now").foregroundStyle(.orange)
                }
                if s.overflowBytes > 0 {
                    Text("Overflow: \(s.overflowBytes) bytes dropped").foregroundStyle(.orange)
                }
                if s.sendErrors > 0 {
                    Text("Send errors: \(s.sendErrors) (errno \(s.lastErrno))").foregroundStyle(.red)
                }
            }
            .font(.callout)
            .foregroundStyle(.secondary)
        }
    }

    private var musicSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Toggle("Apple Music mode", isOn: Binding(get: { music.enabled }, set: { music.setEnabled($0) }))
                .fontWeight(.medium)
            Text("Music plays to ScreamMusic at each track's own sample rate and bit depth; other apps go to another output.")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if let p = music.problem { Text(p).font(.callout).foregroundStyle(.red) }
            if music.enabled {
                let f = music.follower
                if !f.trackName.isEmpty {
                    Text("\(f.trackName)\(f.trackRate.map { " — \(rateText($0))" } ?? "")\(f.trackBits.map { " / \($0)-bit" } ?? "")\(f.lossless ? "" : " (lossy)")")
                        .font(.callout).lineLimit(1)
                }
                if !f.lastAction.isEmpty {
                    Text(f.lastAction + (f.switches > 0 ? "  ·  \(f.switches) switch\(f.switches == 1 ? "" : "es")" : ""))
                        .font(.caption).foregroundStyle(.secondary)
                }
                ForEach(music.musicWarnings, id: \.self) { Text($0).font(.caption).foregroundStyle(.orange) }
                if let o = music.othersStatus {
                    Text(o).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
            Picker("Other apps", selection: $music.othersRoute) {
                Text(music.previousOutputName.isEmpty ? "Output before Music mode" : "Output before Music mode (\(music.previousOutputName))").tag("previous")
                Text("Built-in output").tag("builtin")
                ForEach(SystemAudio.outputDevices(), id: \.self) { d in
                    Text(SystemAudio.name(d)).tag(SystemAudio.uid(d))
                }
                Text("Mute").tag("mute")
            }
            Toggle("Follow each track's bit depth (16 / 24-bit)", isOn: $music.followDepth)
                .font(.callout)
        }
    }

    private var destinationSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Receiver").fontWeight(.medium)
            HStack {
                TextField("IP address", text: $model.edit.ip)
                    .textFieldStyle(.roundedBorder)
                TextField("Port", value: $model.edit.port, format: .number.grouping(.never))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 70)
            }
            Picker("Header", selection: $model.edit.header) {
                Text("Extended (6-byte)").tag("extended")
                Text("Original (5-byte, S32 only)").tag("original")
            }
            HStack {
                if let e = model.applyError {
                    Text(e).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                Button("Apply") { model.applyDestination() }
                    .disabled(model.edit == model.destination)
            }
            if model.edit.header != model.destination.header {
                Text("A header change takes effect when playback next starts.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var advancedSection: some View {
        let s = model.status
        return VStack(alignment: .leading, spacing: 6) {
            Picker("Device sample rate", selection: Binding(get: { model.rate ?? 44100 }, set: { model.setRate($0) })) {
                ForEach(ScreamDevice.rates, id: \.self) { Text(rateText($0)).tag($0) }
            }
            .disabled(music.enabled)
            Text(music.enabled ? "Apple Music mode sets the rate for each track." : "Players in exclusive mode set the rate themselves; this is for testing.")
                .font(.caption).foregroundStyle(.secondary)
            Group {
                Text("Device format: \(s.physicalFormat), \(rateText(model.rate ?? 0))")
                Text("Packets \(s.packets)  ·  end-of-track \(s.endOfTrackMarkers)  ·  hold \(s.holdMode)\(s.holdReached ? " (holding)" : "")")
                if s.othersFrames > 0 { Text("Other apps' frames moved: \(s.othersFrames)") }
                Text("Log: ~/Library/Logs/ScreamMusic.log")
            }
            .font(.caption).foregroundStyle(.secondary)
        }
        .padding(.top, 4)
    }

    private var driverSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                switch installer.state {
                case .notInstalled:
                    Text("Driver not installed").foregroundStyle(.secondary)
                    Spacer()
                    Button("Install Driver…") { installer.install() }
                case .installed(let v):
                    Text("Driver \(v)").foregroundStyle(.secondary)
                    Spacer()
                    Button("Remove Driver…") { installer.remove() }
                    Button("Uninstall…") { confirmUninstall = true }
                case .outdated(let i, let b):
                    Text("Driver \(i) (this app carries \(b))").foregroundStyle(.secondary)
                    Spacer()
                    Button("Update Driver…") { installer.install() }
                }
            }
            .font(.callout)
            .disabled(installer.busy)
            if installer.busy { ProgressView().controlSize(.small) }
            if let e = installer.lastError {
                Text(e).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
