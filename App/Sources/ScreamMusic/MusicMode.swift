//
//  MusicMode.swift
//  ScreamMusic
//
//  Apple Music mode (DESIGN.md §6.2): ScreamMusic becomes the default output, the driver sends
//  Music alone (SMmx), other apps and alert sounds go to another output, and RateFollower keeps
//  the device at each track's format. Undone on switch-off and quit, and at the next launch after
//  a crash.
//

import AppKit
import CoreAudio
import Foundation
import ScreamMusicKit

@MainActor
final class MusicMode: ObservableObject {

    /// Where other apps' sound goes: "previous" (the output before the mode started), "builtin",
    /// "mute", or a device UID.
    @Published var othersRoute: String {
        didSet { UserDefaults.standard.set(othersRoute, forKey: Keys.othersRoute); if enabled { applyOthers() } }
    }
    @Published var followDepth: Bool {
        didSet { UserDefaults.standard.set(followDepth, forKey: Keys.followDepth); if enabled { restartFollower() } }
    }
    @Published private(set) var enabled = false
    @Published private(set) var follower = FollowerState()
    @Published private(set) var othersStatus: String?
    @Published private(set) var problem: String?
    @Published private(set) var previousOutputName = ""
    @Published private(set) var musicWarnings: [String] = []

    private enum Keys {
        static let enabled = "musicMode"
        static let othersRoute = "othersRoute"
        static let followDepth = "followDepth"
        static let previousOutput = "previousOutputUID"
        static let previousAlert = "previousAlertUID"
        static let ownsOutput = "ownsOutput"
    }

    private let others = OthersPlayer()
    private var rateFollower: RateFollower?
    private var device: AudioObjectID?
    private var workspaceObservers: [NSObjectProtocol] = []
    private var rateListener: AudioObjectPropertyListenerBlock?
    private var rateListenerDevice: AudioObjectID = 0

    static let sendDelayMs = 400
    static let defaultDelayMs = 20

    init() {
        let d = UserDefaults.standard
        othersRoute = d.string(forKey: Keys.othersRoute) ?? "previous"
        followDepth = d.object(forKey: Keys.followDepth) as? Bool ?? true
        previousOutputName = d.string(forKey: Keys.previousOutput).flatMap { SystemAudio.device(uid: $0) }.map { SystemAudio.name($0) } ?? ""
    }

    var wantsEnabled: Bool { UserDefaults.standard.bool(forKey: Keys.enabled) }

    /// At launch: undo what a crashed run left behind, then resume the mode if it was on.
    func launch() {
        let d = UserDefaults.standard
        if d.bool(forKey: Keys.ownsOutput) && !wantsEnabled {
            elog("music mode: the last run did not clean up; restoring the outputs")
            restoreOutputs()
            if let dev = ScreamDevice.find() { clearDriver(dev) }
        }
        if wantsEnabled { setEnabled(true) }
    }

    func setEnabled(_ on: Bool) {
        UserDefaults.standard.set(on, forKey: Keys.enabled)
        if on { enable() } else { disable() }
    }

    /// App quit: give the outputs back, but keep the mode on for the next launch.
    func quit() {
        if enabled { tearDown() }
    }

    /// The driver was reloaded (coreaudiod restarted, driver updated): the device has a new object ID
    /// and the driver starts with Apple Music mode off. Called every second by AppModel.
    func checkDriver() {
        guard enabled else {
            // the mode is wanted but the driver wasn't there: start when it appears
            if wantsEnabled, problem != nil, ScreamDevice.find() != nil { enable() }
            return
        }
        let dev = ScreamDevice.find()
        let ours = dev.flatMap { ScreamDevice.getInt($0, ScreamDevice.owner) } == Int(getpid())
        guard dev != device || !ours else { return }
        elog("music mode: the driver was reloaded (device \(device.map(String.init) ?? "-") -> \(dev.map(String.init) ?? "-")); attaching again")
        rateFollower?.stop()
        rateFollower = nil
        others.stop()
        removeRateListener()
        for o in workspaceObservers { NSWorkspace.shared.notificationCenter.removeObserver(o) }
        workspaceObservers = []
        enabled = false
        guard dev != nil else {
            problem = "The ScreamMusic driver is not loaded."
            return
        }
        enable()
    }

    // MARK: -

    private func enable() {
        guard !enabled else { return }
        problem = nil
        guard let dev = ScreamDevice.find() else {
            problem = "The ScreamMusic driver is not loaded."
            return
        }
        device = dev
        let d = UserDefaults.standard

        // remember the outputs to give back; if ScreamMusic already is the default, keep what we had
        if let cur = SystemAudio.defaultOutput(), cur != dev {
            d.set(SystemAudio.uid(cur), forKey: Keys.previousOutput)
        } else if d.string(forKey: Keys.previousOutput) == nil, let b = SystemAudio.builtInOutput() {
            d.set(SystemAudio.uid(b), forKey: Keys.previousOutput)
        }
        if let a = SystemAudio.alertOutput(), a != dev {
            d.set(SystemAudio.uid(a), forKey: Keys.previousAlert)
        }
        previousOutputName = d.string(forKey: Keys.previousOutput).flatMap { SystemAudio.device(uid: $0) }.map { SystemAudio.name($0) } ?? ""
        d.set(true, forKey: Keys.ownsOutput)

        ScreamDevice.setInt(dev, ScreamDevice.owner, Int(getpid()))
        ScreamDevice.setInt(dev, ScreamDevice.sendDelay, Self.sendDelayMs)
        updateMusicPID()
        SystemAudio.setDefaultOutput(dev)
        elog("music mode: on; default output ScreamMusic (was \(previousOutputName))")

        let ws = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.didLaunchApplicationNotification, NSWorkspace.didTerminateApplicationNotification] {
            workspaceObservers.append(ws.addObserver(forName: name, object: nil, queue: .main) { [weak self] n in
                guard (n.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)?.bundleIdentifier == "com.apple.Music" else { return }
                Task { @MainActor in self?.updateMusicPID() }
            })
        }
        listenForRateChanges(dev)
        enabled = true
        applyOthers()
        restartFollower()
        checkMusicSettings()
    }

    private func disable() {
        guard enabled else { return }
        tearDown()
        elog("music mode: off")
    }

    private func tearDown() {
        rateFollower?.stop()
        rateFollower = nil
        others.stop()
        othersStatus = nil
        for o in workspaceObservers { NSWorkspace.shared.notificationCenter.removeObserver(o) }
        workspaceObservers = []
        removeRateListener()
        if let dev = device { clearDriver(dev) }
        restoreOutputs()
        enabled = false
    }

    private func clearDriver(_ dev: AudioObjectID) {
        ScreamDevice.setInt(dev, ScreamDevice.musicPID, 0)
        ScreamDevice.setInt(dev, ScreamDevice.hold, ScreamDevice.Hold.run.rawValue)
        ScreamDevice.setInt(dev, ScreamDevice.sendDelay, Self.defaultDelayMs)
        ScreamDevice.setInt(dev, ScreamDevice.owner, 0)
    }

    private func restoreOutputs() {
        let d = UserDefaults.standard
        let scream = ScreamDevice.find()
        let back = d.string(forKey: Keys.previousOutput).flatMap { SystemAudio.device(uid: $0) } ?? SystemAudio.builtInOutput()
        if let b = back, SystemAudio.defaultOutput() == scream || scream == nil {
            SystemAudio.setDefaultOutput(b)
            elog("music mode: default output back to \(SystemAudio.name(b))")
        }
        if let a = d.string(forKey: Keys.previousAlert).flatMap({ SystemAudio.device(uid: $0) }) {
            SystemAudio.setAlertOutput(a)
        }
        d.set(false, forKey: Keys.ownsOutput)
    }

    /// A pid no audio client has. Drivers through 0.4.2 treat 0 as "send every client", so quitting
    /// Music used to stop diverting other apps (and send them at the receiver, which may be down).
    private static let musicAbsentPID = -1

    private func updateMusicPID() {
        guard let dev = device else { return }
        let music = NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.Music").first?.processIdentifier ?? 0
        let pid = music == 0 ? Self.musicAbsentPID : Int(music)
        ScreamDevice.setInt(dev, ScreamDevice.musicPID, pid)
        elog("music mode: Music \(music == 0 ? "not running; other apps still diverted" : "pid \(music)")")
    }

    func clearRateLibrary() {
        RateLibrary.shared.removeAll()
        rateFollower?.refreshLibraryCount()
        follower.libraryCount = 0
        elog("rate library: cleared")
    }

    private func restartFollower() {
        rateFollower?.stop()
        guard enabled, let dev = device else { return }
        let f = RateFollower(device: dev, followDepth: followDepth)
        f.onChange = { [weak self] s in
            self?.follower = s
            self?.checkMusicSettings()
        }
        rateFollower = f
        f.start()
    }

    // MARK: - other apps

    /// The output other apps go to, per the route.
    func othersTarget() -> AudioObjectID? {
        switch othersRoute {
        case "mute": return nil
        case "builtin": return SystemAudio.builtInOutput()
        case "previous":
            return UserDefaults.standard.string(forKey: Keys.previousOutput).flatMap { SystemAudio.device(uid: $0) } ?? SystemAudio.builtInOutput()
        default:
            return SystemAudio.device(uid: othersRoute) ?? SystemAudio.builtInOutput()
        }
    }

    private func applyOthers() {
        others.stop()
        guard enabled, let dev = device else { return }
        guard let target = othersTarget() else {
            othersStatus = "Other apps: muted"
            return
        }
        SystemAudio.setAlertOutput(target)
        Task { @MainActor in
            guard await OthersPlayer.microphoneAllowed() else {
                othersStatus = "Other apps are muted: allow ScreamMusic to use the Microphone in System Settings › Privacy & Security."
                return
            }
            guard enabled else { return }
            if let err = others.start(scream: dev, target: target) {
                othersStatus = err
            } else {
                othersStatus = "Other apps → \(others.targetName)"
            }
        }
    }

    /// The others player converts from ScreamMusic's rate: restart it when a track switch changes it.
    private func listenForRateChanges(_ dev: AudioObjectID) {
        var addr = ScreamDevice.address(kAudioDevicePropertyNominalSampleRate)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            Task { @MainActor in
                guard let self, self.enabled, self.others.isRunning else { return }
                self.applyOthers()
            }
        }
        rateListener = block
        rateListenerDevice = dev
        AudioObjectAddPropertyListenerBlock(dev, &addr, .main, block)
    }

    private func removeRateListener() {
        guard let block = rateListener else { return }
        var addr = ScreamDevice.address(kAudioDevicePropertyNominalSampleRate)
        AudioObjectRemovePropertyListenerBlock(rateListenerDevice, &addr, .main, block)
        rateListener = nil
    }

    // MARK: - Music settings that change samples

    private func checkMusicSettings() {
        guard enabled, !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.Music").isEmpty else {
            musicWarnings = []
            return
        }
        Task.detached {
            let s = MusicScripts()
            var w: [String] = []
            if let v = s.volume(), v < 100 { w.append("Music's volume is \(v): set it to 100 for bit-perfect output.") }
            if s.eqEnabled() == true { w.append("Music's EQ is on: turn it off for bit-perfect output.") }
            await MainActor.run { [w] in self.musicWarnings = w }
        }
    }
}
