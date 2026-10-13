//
//  AppModel.swift
//  ScreamMusic
//
//  Polls the driver's status and holds what the menu-bar panel edits.
//

import CoreAudio
import Foundation

@MainActor
final class AppModel: ObservableObject {

    @Published private(set) var device: AudioObjectID?
    @Published private(set) var status = DriverStatus()
    @Published private(set) var rate: Double?
    @Published private(set) var hogOwner: pid_t?
    /// The driver's current destination; `edit` is what the panel shows until Apply.
    @Published private(set) var destination = Destination()
    @Published var edit = Destination()
    @Published private(set) var applyError: String?

    // MARK: - bit-perfect badge

    /// The driver's run counters start at the last format change, so switching to 96 kHz leaves a few
    /// thousand off-grid samples from the tail of the old run, and every later same-format track would
    /// look dirty. Instead the badge follows the samples of the last second: clean for ~2 s straight
    /// (after a track starts or a switch settles) means the current audio is on the grid.
    enum BitPerfectState: Equatable {
        case idle
        case checking
        case perfect(since: Date)
        case degraded(offGrid: Int)
    }
    @Published private(set) var bitPerfect: BitPerfectState = .idle
    private var lastRun: (frames: Int, offGrid: Int)?
    private var cleanWindows = 0

    // MARK: - underrun badge

    /// The driver's underrun counter only ever grows (~1 per ms of stall), so the panel shows the
    /// recent stalls instead: each increment is about a millisecond of the sender waiting.
    @Published private(set) var underrunMs = 0
    private var lastUnderruns: Int?
    private var underrunQuiet = 0

    private var isDegraded: Bool {
        if case .degraded = bitPerfect { return true }
        return false
    }

    /// Every poll (1 s). A growing counter means the sender just waited for audio; show the recent
    /// stalls and hide them again after a few quiet seconds.
    private func updateUnderruns() {
        let u = status.underruns
        defer { lastUnderruns = u }
        guard let last = lastUnderruns else { return }
        let delta = u - last
        if delta > 0 {
            underrunMs += delta
            underrunQuiet = 0
        } else {
            underrunQuiet += 1
            if underrunQuiet >= 3 { underrunMs = 0 }
        }
    }


    let installer = DriverInstaller()
    let musicMode = MusicMode()
    private var timer: Timer?

    init() {
        // After the first frame. poll()/launch() call into coreaudiod, which is busy for a minute
        // or more after a driver install; doing that before the window exists looks like a hang.
        DispatchQueue.main.async { [weak self] in self?.start() }
    }

    private func start() {
        poll()
        edit = destination
        musicMode.launch()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func poll() {
        musicMode.checkDriver()
        let found = ScreamDevice.find()
        if found != device {
            device = found
            installer.refresh()
        }
        guard let d = found else {
            status = DriverStatus()
            rate = nil
            hogOwner = nil
            bitPerfect = .idle
            lastRun = nil
            cleanWindows = 0
            return
        }
        status = DriverStatus(ScreamDevice.get(d, ScreamDevice.status)) ?? DriverStatus()
        rate = ScreamDevice.nominalRate(d)
        hogOwner = ScreamDevice.hogOwner(d)
        updateBitPerfect()
        updateUnderruns()
        if let dest = Destination(ScreamDevice.get(d, ScreamDevice.destination)), dest != destination {
            let editing = edit != destination
            destination = dest
            if !editing { edit = dest }
        }
    }

    func applyDestination() {
        guard let d = device else { return }
        let st = ScreamDevice.set(d, ScreamDevice.destination, edit.plist)
        applyError = st == noErr ? nil : "The driver refused this destination (\(st)). Check the IP address and port."
        poll()
        if st == noErr { edit = destination }
    }

    func setLeadIn(_ ms: Int) {
        guard let d = device else { return }
        ScreamDevice.setInt(d, ScreamDevice.leadIn, ms)
        poll()
    }

    func setRate(_ r: Double) {
        guard let d = device else { return }
        ScreamDevice.setNominalRate(d, r)
        poll()
    }

    /// Removes everything: Apple Music mode off and outputs restored, the driver (one administrator
    /// prompt), this app's preferences and logs, then the app itself (when it sits somewhere
    /// user-deletable). `finish` runs on the main thread with whether the app should also delete
    /// itself (false: the user has to move it to the Trash).
    func uninstall(finish: @escaping (Bool) -> Void) {
        musicMode.setEnabled(false)
        installer.remove { ok in
            guard ok else { return }   // cancelled or failed: the app stays
            if let id = Bundle.main.bundleIdentifier {
                UserDefaults.standard.removePersistentDomain(forName: id)
            }
            let fm = FileManager.default
            let logs = fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs")
            for name in ["ScreamMusic.log", "ScreamMusic.1.log", "ScreamMusic.2.log"] {
                try? fm.removeItem(at: logs.appendingPathComponent(name))
            }
            let app = Bundle.main.bundleURL
            if fm.isDeletableFile(atPath: app.path) {
                let escaped = app.path.replacingOccurrences(of: "'", with: "'\\''")
                let p = Process()
                p.executableURL = URL(fileURLWithPath: "/bin/sh")
                // after the app has quit
                p.arguments = ["-c", "sleep 1; rm -rf '\(escaped)'"]
                try? p.run()
                finish(true)
            } else {
                finish(false)
            }
        }
    }

    private func updateBitPerfect() {
        let s = status
        guard s.running else {
            bitPerfect = .idle
            lastRun = nil
            cleanWindows = 0
            return
        }
        let cur = (frames: s.runFrames, offGrid: s.runOffGridSamples)
        guard let last = lastRun else {
            // first look at this run: wait for a window
            lastRun = cur
            cleanWindows = 0
            if bitPerfect == .idle { bitPerfect = .checking }
            return
        }
        defer { lastRun = cur }
        let framesDelta = cur.frames - last.frames
        if framesDelta <= 0 {
            // paused, or the run restarted (format change): the next window decides
            return
        }
        // a window counts only if it carried real audio (not just idle silence padding)
        let enoughFrames = framesDelta >= Int(s.rate) / 3   // ~0.33 s of frames
        let offDelta = cur.offGrid - last.offGrid
        if offDelta == 0 && enoughFrames {
            cleanWindows += 1
            if cleanWindows >= 2 {
                if case .perfect = bitPerfect {
                    // still perfect; keep the original "since"
                } else {
                    bitPerfect = .perfect(since: Date())
                }
            } else if bitPerfect == .idle || isDegraded {
                bitPerfect = .checking
            }
        } else if offDelta > 0 {
            cleanWindows = 0
            bitPerfect = .degraded(offGrid: offDelta)
        }
    }
}
