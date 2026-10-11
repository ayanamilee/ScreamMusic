//
//  MusicScripts.swift
//  ScreamMusic
//
//  AppleScript for Music (Automation permission). Used only on the follower's queue: Music answers
//  Apple events on the thread that runs its playback controls, so calls are kept few.
//  Script texts as in Nativerate's RendererScripts (GPL-3.0).
//

import Foundation

final class MusicScripts {
    private let pauseScript = compile("tell application \"Music\" to pause")
    private let playScript = compile("tell application \"Music\" to play")
    private let positionScript = compile("tell application \"Music\" to get player position")
    private let remainingScript = compile("tell application \"Music\" to get (duration of current track) - player position")
    private let volumeScript = compile("tell application \"Music\" to get sound volume")
    private let stateScript = compile("tell application \"Music\" to get player state as string")
    private let eqScript = compile("tell application \"Music\" to get EQ enabled")
    private let trackRateScript = compile("tell application \"Music\" to get {name, sample rate} of current track")

    private static func compile(_ source: String) -> NSAppleScript? {
        let s = NSAppleScript(source: source)
        var err: NSDictionary?
        s?.compileAndReturnError(&err)
        if let err { elog("AppleScript compile error: \(err)") }
        return s
    }

    private func run(_ s: NSAppleScript?) -> NSAppleEventDescriptor? {
        var err: NSDictionary?
        let d = s?.executeAndReturnError(&err)
        if let err {
            elog("AppleScript error: \(err[NSAppleScript.errorMessage] ?? err)")
            return nil
        }
        return d
    }

    @discardableResult func pause() -> Bool { run(pauseScript) != nil }
    @discardableResult func play() -> Bool { run(playScript) != nil }
    func position() -> Double? { run(positionScript)?.doubleValue }
    func remaining() -> Double? { run(remainingScript)?.doubleValue }
    func volume() -> Int? { run(volumeScript).map { Int($0.int32Value) } }
    func playerState() -> String? { run(stateScript)?.stringValue }
    func eqEnabled() -> Bool? { run(eqScript)?.booleanValue }

    /// The current track's name and sample rate as Music reports them (rate nil: "missing value",
    /// as right at a track change).
    func trackRate() -> (name: String, rate: Double?)? {
        guard let d = run(trackRateScript), d.numberOfItems >= 2 else { return nil }
        let r = d.atIndex(2).map { $0.doubleValue } ?? 0
        return (d.atIndex(1)?.stringValue ?? "", r > 0 ? r : nil)
    }

    @discardableResult func setPosition(_ p: Double) -> Bool {
        run(Self.compile("tell application \"Music\" to set player position to \(p)")) != nil
    }
}
