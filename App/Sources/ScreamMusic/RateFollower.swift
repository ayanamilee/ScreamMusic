//
//  RateFollower.swift
//  ScreamMusic
//
//  Apple Music mode: keeps the ScreamMusic device at the playing track's sample rate (and the
//  wire at its bit depth) so Music's samples are never resampled. DESIGN.md §6.2.
//
//  Music renders at whatever rate the device runs; it can't be made to wait from the device side.
//  So, when a track needs another format:
//    - the driver holds the stream where the old track ends (its inter-track zeros: SMhd boundary,
//      armed 1.5 s before the end once Music's decoder pre-roll for the next track shows another
//      rate, or right away on a skip), or at the first sound after a pause (SMhd gate);
//    - on the new track's Playing: pause Music, drop what was rendered at the old rate (SMhd drop,
//      which also sends the end-of-track marker), switch the device, rewind to where the track
//      began, play.
//  Same-format changes (gapless albums), pause/resume and seeks pass through untouched.
//
//  Sources for the track's format, as Nativerate (GPL-3.0) found them: Music's own sample rate for
//  the current track (AppleScript) first; the decoder's "Input format: ... N Hz ... from B-bit
//  source" log lines for the depth, and for the rate when Music doesn't say.
//
//  Apple Music streams (HLS) start on a lower variant for the first ~2-3 s (seen: 48 kHz, then the
//  96 kHz/24-bit variant at 2.56 s, inside one player item), and Music's AppleScript rate can be the
//  start variant's. After a seek Music also sets up the next track's pre-roll next to the current
//  track's decoder, and a decoder line does not say which track it is for. So inside a track decoder
//  lines never switch by themselves: they are candidates, and the switch comes from the samples (the
//  grid check): at the device's format a lossless track lands on the wire's grid; off it means
//  Music went to another variant, and the device moves there without a rewind (a rewind replays the
//  start segment, and would loop).
//
//  Runs on its own serial queue; waits there block nothing else.
//

import AppKit
import CoreAudio
import Foundation
import ScreamMusicKit

struct FollowerState: Equatable {
    var trackName = ""
    var trackRate: Double?
    var trackBits: Int?
    var lossless = true
    var atmos = false
    /// where the format came from: "rate library", "pre-roll", "its decoder line", "Music's rate", ...
    var source = ""
    var lastAction = ""
    var switches = 0
    var libraryCount = 0
}

final class RateFollower: @unchecked Sendable {

    var onChange: ((FollowerState) -> Void)?

    private let q = DispatchQueue(label: "ScreamMusic.follower", qos: .userInitiated)
    private let scripts = MusicScripts()
    private let device: AudioObjectID
    private var logProcess: Process?
    private var observer: NSObjectProtocol?

    // queue state
    private var state = FollowerState()
    private var curRate: Double = 0
    private var curBits = 24
    private var playing = false
    private var lastTrackID: Int64?
    private var lastNewTrackAt: Date?
    private var ownLinesUntil: Date?
    private var lines: [(date: Date, rate: Double, bits: Int?, lossless: Bool)] = []
    private var awaiting: (name: String, tPlay: Date, until: Date, scriptRate: Double?)?
    /// The format the device was set to for the current track; decoder lines inside the track only
    /// move it up, the grid check moves it either way.
    private var trackTarget: Fmt?
    /// Lossless formats the current track's own decoder lines showed, newest last.
    private var trackFormats: [Fmt] = []
    /// Formats seen just after a seek or resume, newest last: Music then sets up the current track's
    /// decoder and the next track's pre-roll together, so these may be either. Grid check only.
    private var maybeFormats: [Fmt] = []
    private var lastSeekAt: Date?
    /// Grid check: the driver's run counters at the last look, bad windows in a row, corrections made
    /// for this track, and when the device format last changed (the check waits for it to settle).
    private var gridLast: (frames: Int, offGrid: Int)?
    private var gridBad = 0
    private var gridFixes = 0
    private var gridTried: [Fmt] = []
    private var formatChangedAt = Date.distantPast
    /// The playing track's Rate Library key (nil: not identified).
    private var currentKey: String?
    /// The current track is lossy (AAC) or Atmos: the grid check stays out, the library learns lossy.
    private var trackLossy = false
    private var trackAtmos = false
    /// An AAC decoder line was seen; the track is only called lossy if no lossless line of its own
    /// shows within 10 s of its start (streams often open AAC and swap in ALAC a second later).
    private var aacSeenAt: Date?
    /// The Rate Library chose this track's start format / is only a grid-check candidate for it.
    /// If the grid check then has to correct or give up, the entry was wrong: forget it.
    private var libraryStart = false
    private var libraryCandidate = false
    /// Learning: frames of clean windows since the last format change / track start.
    private var learnCleanFrames = 0

    typealias Fmt = WireFormat
    private var inSwitch = false
    private var armed: ScreamDevice.Hold = .run
    private var reachedSeenAt: Date?
    private var armSerial = 0
    private var stopped = false

    init(device: AudioObjectID) {
        self.device = device
    }

    // MARK: - lifecycle

    func start() {
        startLogStream()
        observer = DistributedNotificationCenter.default().addObserver(forName: Notification.Name("com.apple.Music.playerInfo"), object: nil, queue: nil) { [weak self] n in
            let info = n.userInfo ?? [:]
            let at = Date()
            self?.q.async { self?.handleInfo(info, at: at) }
        }
        RateLibrary.shared.setErrorHandler { elog($0) }
        gridTick()
        q.async { [self] in
            curRate = ScreamDevice.nominalRate(device) ?? 44100
            curBits = ScreamDevice.getInt(device, ScreamDevice.wireBits).flatMap { $0 == 0 ? nil : $0 } ?? 24
            elog("follower: started; device at \(Int(curRate)) Hz, wire \(curBits)-bit")
            guard musicRunning() else { setHold(.gate); watchGate(); return }
            if scripts.playerState() == "playing" {
                // already playing: treat the current track as new (a short rewind if it needs a switch)
                playing = true
                let name = scripts.trackRate()?.name ?? ""
                newTrack(name: name, id: nil, at: Date())
            } else {
                setHold(.gate)
                watchGate()
            }
        }
    }

    /// The library changed from outside (cleared in the UI): show its size.
    func refreshLibraryCount() {
        q.async { [self] in
            state.libraryCount = RateLibrary.shared.count
            publish()
        }
    }

    func stop() {
        if let o = observer { DistributedNotificationCenter.default().removeObserver(o) }
        observer = nil
        logProcess?.terminate()
        logProcess = nil
        q.sync {
            stopped = true
            finalizeLearning()
            armSerial += 1
            ScreamDevice.setInt(device, ScreamDevice.hold, ScreamDevice.Hold.run.rawValue)
            armed = .run
        }
        RateLibrary.shared.flush()
        elog("follower: stopped")
    }

    // MARK: - inputs

    private func startLogStream() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        // decoder setups, and seeks: a seek sets the current track's decoder up again (and Music posts
        // no playerInfo for it), so the lines after it are that track's, not the next one's pre-roll
        p.arguments = ["stream", "--style", "compact", "--predicate",
                       "process == \"Music\" AND (eventMessage CONTAINS \"Input format:\" OR eventMessage CONTAINS \"SetCurrentTimeWithRangeAndSeekIDGuts\")"]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        var partial = ""   // touched only by the serial readability handler
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            guard let chunk = String(data: h.availableData, encoding: .utf8), !chunk.isEmpty else { return }
            partial += chunk
            var parts = partial.components(separatedBy: "\n")
            partial = parts.removeLast()
            let now = Date()
            for line in parts {
                if line.contains("Input format:") {
                    self?.q.async { self?.handleLine(line, at: now) }
                } else if line.contains("Seeking to") {
                    self?.q.async { self?.handleSeek(line, at: now) }
                }
            }
        }
        do {
            try p.run()
            logProcess = p
        } catch {
            elog("follower: cannot start log stream: \(error)")
        }
    }

    private func musicRunning() -> Bool {
        !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.Music").isEmpty
    }

    // MARK: - player notifications

    private func handleInfo(_ info: [AnyHashable: Any], at: Date) {
        guard !stopped else { return }
        let st = info["Player State"] as? String ?? "?"
        let name = info["Name"] as? String ?? ""
        // stations and Browse streams have no PersistentID: tell tracks apart by name
        let id = (info["PersistentID"] as? NSNumber)?.int64Value ?? (name.isEmpty ? nil : Int64(truncatingIfNeeded: name.hashValue))
        elog("playerInfo: \(st) \(name)")
        playing = st == "Playing"
        if inSwitch { return }     // our own pause/play
        if playing, name.isEmpty, info["PersistentID"] == nil {
            elog("playerInfo without a name or PersistentID: not a track")
            return
        }
        if !playing {
            // the next play waits at the gate until its track's format is known
            setHold(.gate)
            watchGate()
            return
        }
        if id == lastTrackID {     // resume or seek
            // Music sets the track's decoder up again: the lines that follow are its own, not a pre-roll
            ownLinesUntil = max(ownLinesUntil ?? .distantPast, at.addingTimeInterval(2))
            release("same track")
            return
        }
        newTrack(name: name, id: id, at: at)
    }

    private func newTrack(name: String, id: Int64?, at: Date) {
        finalizeLearning()
        lastTrackID = id
        let prev = lastNewTrackAt
        let prevOwn = ownLinesUntil
        lastNewTrackAt = at
        ownLinesUntil = max(at.addingTimeInterval(2), prevOwn ?? .distantPast)
        awaiting = nil
        trackTarget = nil
        trackFormats = []
        maybeFormats = []
        gridBad = 0
        gridFixes = 0
        gridTried = []
        trackLossy = false
        trackAtmos = false
        aacSeenAt = nil
        libraryStart = false
        libraryCandidate = false
        learnCleanFrames = 0
        state.trackName = name

        // The next track's pre-roll: lossless lines after the previous track's own window (a stream can
        // show both its variants there). With one, this is a natural transition: decide now.
        let afterPrev = prev.map { $0.addingTimeInterval(Self.trackWindow) } ?? .distantPast
        let preRoll = lines.filter { $0.date > afterPrev && $0.date > (prevOwn ?? .distantPast) && $0.date <= at && $0.lossless && $0.bits != nil }
        let info = musicTrackInfo(name: name)
        let scriptRate = info?.rate
        var learned: LearnedFormat?
        if let key = info?.key {
            currentKey = key
            learned = RateLibrary.shared.lookup(key)
        }
        // A pre-roll line (natural transition) outranks the library, which then only backs the grid
        // check; a manual start has no pre-roll, so the library saves the wait for the decoder line.
        let preRollBest = preRoll.compactMap({ fmt(rate: $0.rate, bits: $0.bits) }).reduce(nil, { Fmt.best($0, $1) })
        if let choice = chooseStart(preRoll: preRollBest, scriptRate: scriptRate, learned: learned) {
            if let c = choice.candidate {
                maybeFormats.append(c)
                libraryCandidate = true
            }
            libraryStart = choice.source == .library
            decide(choice.target, name: name, tPlay: at, how: choice.source.rawValue, manualStart: choice.manualStart)
            return
        }
        // A manual start (picked, skipped, played from pause): no pre-roll. Hold where it begins and wait
        // briefly for its own decoder line; Music's rate alone can be a stream's start variant.
        awaiting = (name, at, at.addingTimeInterval(Self.startWait), scriptRate)
        if armed == .run { arm(.boundary, expiry: 4) }
        elog("new track \(name): no pre-roll\(scriptRate.map { "; Music says \(Int($0)) Hz" } ?? ""); holding, waiting up to \(Self.startWait) s for its decoder")
        q.asyncAfter(deadline: .now() + Self.startWait + 0.05) { [weak self] in
            guard let self, let aw = self.awaiting, Date() >= aw.until else { return }
            self.awaiting = nil
            if let r = aw.scriptRate {
                self.decide(Fmt(rate: r, bits: r == self.curRate ? self.curBits : 24), name: aw.name, tPlay: aw.tPlay, how: "Music's rate, no decoder line", manualStart: true)
            } else {
                elog("new track \(aw.name): format unknown after \(Self.startWait) s; playing on at \(Int(self.curRate)) Hz")
                self.state.source = "unknown"
                self.release("format unknown")
            }
        }
    }

    /// The wire format for a lossless line, nil for a 'qlac' line without the depth.
    private func fmt(rate: Double, bits: Int?) -> Fmt? {
        guard let b = Self.wireBits(bits) else { return nil }
        return Fmt(rate: rate, bits: b)
    }

    /// The current track as Music reports it, if it is `name` (~1 s of retries: it reads
    /// "missing value", or still the previous track, right at a change). Also the track's Rate
    /// Library key: its persistent ID, else artist — album — name.
    private func musicTrackInfo(name: String) -> (rate: Double?, key: String?)? {
        guard musicRunning() else { return nil }
        var last: (rate: Double?, key: String?)?
        for attempt in 0..<6 {
            if attempt > 0 { Thread.sleep(forTimeInterval: 0.15) }
            guard let t = scripts.trackInfo() else { continue }
            if !name.isEmpty, t.name != name { continue }
            let key: String?
            if !t.persistentID.isEmpty {
                key = "pid:" + t.persistentID
            } else if t.artist.isEmpty && t.album.isEmpty {
                key = nil      // a bare title would collide with every other bare title (radio, streams)
            } else {
                let k = RateLibrary.key(name: t.name, artist: t.artist, album: t.album)
                key = k.isEmpty ? nil : k
            }
            last = (t.rate, key)
            if t.rate != nil { return last }      // "missing value" right at a change: ask again
        }
        return last
    }

    // MARK: - decoder lines

    private func handleSeek(_ line: String, at: Date) {
        guard !stopped else { return }
        lastSeekAt = at
        ownLinesUntil = max(ownLinesUntil ?? .distantPast, at.addingTimeInterval(2))
        if !inSwitch, let r = line.range(of: #"Seeking to [0-9.]+ from [0-9.]+"#, options: .regularExpression) {
            elog("seek: \(line[r].dropFirst(8))")
        }
    }

    private func handleLine(_ line: String, at: Date) {
        guard !stopped, let r = line.range(of: "Input format:") else { return }
        let rest = line[r.upperBound...]
        guard let hz = rest.range(of: #"[0-9]+ Hz"#, options: .regularExpression), let rate = Double(rest[hz].dropLast(3)) else { return }
        // codec from the decoder's file name / format tag: alac/qlac = lossless, aac = lossy,
        // Atmos = rendered (spatial), never bit-perfect as stereo
        let lossless = line.contains("alac") || line.contains("qlac")
        let aac = line.contains("aac")
        let atmos = line.contains("Atmos")
        let bits = rest.range(of: #"from [0-9]+-bit source"#, options: .regularExpression).flatMap { Int(rest[$0].dropFirst(5).prefix { $0.isNumber }) }
        if lines.last?.rate != rate || lines.last?.bits != bits || at.timeIntervalSince(lines.last!.date) > 0.5 {
            elog("decoder: \(Int(rate)) Hz\(bits.map { " \($0)-bit" } ?? "") (\(atmos ? "atmos" : lossless ? "lossless" : aac ? "aac" : "lossy"))")
        }
        lines.append((at, rate, bits, lossless))
        if lines.count > 200 { lines.removeFirst(100) }
        if inSwitch { return }

        if atmos {
            // rendered output: nothing on the wire can be bit-perfect; note it and leave it alone
            if playing && !trackAtmos, let started = lastNewTrackAt, at.timeIntervalSince(started) < Self.trackWindow {
                trackAtmos = true
                state.atmos = true
                state.lossless = false
                publish()
                elog("decoder: Dolby Atmos rendering for \(state.trackName): not bit-perfect")
            }
            return
        }
        if aac {
            // a lossy decoder: the samples are AAC reconstructions, never on the grid. But a stream
            // can open AAC and swap in ALAC a second later, so only call the track lossy when no
            // lossless line of its own shows within 10 s of its start.
            if let aw = awaiting {
                awaiting = nil
                decide(Fmt(rate: rate, bits: 24), name: aw.name, tPlay: aw.tPlay, how: "its decoder line (AAC)", manualStart: true, lossy: true)
                return
            }
            if playing {
                aacSeenAt = at
                elog("decoder: \(Int(rate)) Hz AAC (waiting \(Int(Self.trackWindow)) s for a lossless line before calling it lossy)")
                let id = lastTrackID
                let deadline = (lastNewTrackAt ?? at).addingTimeInterval(Self.trackWindow + 0.2)
                q.asyncAfter(deadline: .now() + max(0, deadline.timeIntervalSinceNow)) { [weak self] in
                    guard let self, !self.stopped, self.playing, self.lastTrackID == id,
                          self.aacSeenAt != nil, !self.trackLossy,
                          self.trackFormats.isEmpty, self.maybeFormats.isEmpty else { return }
                    self.trackLossy = true
                    self.state.lossless = false
                    self.publish()
                    elog("decoder: \(self.state.trackName) is AAC (lossy)")
                }
            }
            return
        }

        let line = fmt(rate: rate, bits: bits)

        // lossless after a lossy start (a stream swapping in its ALAC, or a track the library knows
        // as lossy playing lossless now): stop treating it as lossy; the grid check works again.
        // Only this track's own lines count: while awaiting its decoder, or inside its window.
        let sinceStart0 = lastNewTrackAt.map { at.timeIntervalSince($0) } ?? .infinity
        let ownWindow0 = awaiting != nil || (sinceStart0 < Self.trackWindow) || (ownLinesUntil.map { at <= $0 } ?? false)
        if (trackLossy || aacSeenAt != nil) && ownWindow0 {
            trackLossy = false
            aacSeenAt = nil
            if !trackAtmos { state.lossless = true }
            publish()
            elog("decoder: \(state.trackName) upgraded to lossless")
        }

        if let aw = awaiting {
            guard let f = line else { return }      // a 'qlac' line without the depth: its partner follows
            awaiting = nil
            let target = Fmt.best(f, aw.scriptRate.map { Fmt(rate: $0, bits: f.bits) })!
            decide(target, name: aw.name, tPlay: aw.tPlay, how: "its decoder line", manualStart: true)
            return
        }

        // A line in the current track's own window: its setup after Playing, a stream's switch to its
        // higher variant, its decoder set up again after a resume/seek, or the next track's pre-roll
        // (set up together with it after a seek). A line cannot tell which, so inside a track it never
        // switches by itself: it is a candidate, and the grid check switches when the samples show the
        // device format is wrong.
        let sinceStart = lastNewTrackAt.map { at.timeIntervalSince($0) } ?? .infinity
        let inTrackWindow = sinceStart >= 0 && sinceStart < Self.trackWindow
        if inTrackWindow || (ownLinesUntil.map { at <= $0 } ?? false) {
            guard let f = line else { return }
            trackFormats.removeAll { $0 == f }
            trackFormats.append(f)
            // record the candidate; the grid check will use it if the samples don't match the device
            maybeFormats.removeAll { $0 == f }
            maybeFormats.append(f)
            return
        }

        // another format while playing: the next track's pre-roll (8-12 s before the end) or a skip.
        // Natural transition: switch at the boundary. A skip is caught when its Playing comes.
        guard playing, armed == .run, let next = line, next != Fmt(rate: curRate, bits: curBits) else { return }
        let left = musicRunning() ? (scripts.remaining() ?? 0) : 0
        if left > Self.preRollLeft {
            // far from the end: not a pre-roll but a track the user picked, whose decoder comes up a
            // moment before its Playing. Hold at the cut now, while it is still ahead of the sender
            // (400 ms behind), so nothing of the new track goes out at the old rate.
            arm(.boundary, expiry: 2)
            elog("a \(Int(rate)) Hz line with \(String(format: "%.1f", left)) s left: probably a skip; boundary armed now")
            return
        }
        scheduleArm(after: max(0, left - 1.5))
        elog("next track needs \(Int(rate)) Hz; \(String(format: "%.1f", left)) s left: boundary armed in \(String(format: "%.1f", max(0, left - 1.5))) s")
    }

    /// Music sets the next track's decoder up 8-12 s before the end; a line with more left than this
    /// belongs to a track about to start now.
    private static let preRollLeft: TimeInterval = 20

    /// How long after a track's Playing its own decoder lines may still come (a stream's higher
    /// variant: ~2-3 s in, later on a slow network).
    private static let trackWindow: TimeInterval = 10
    /// After the play command: lets what the seek left in Music's buffers flow into the drop.
    private static let rewindSettle: TimeInterval = 0.08
    /// The grid check's window.
    private static let gridInterval: TimeInterval = 0.5
    /// How long a manual start waits for the track's decoder line.
    private static let startWait: TimeInterval = 1.5

    /// Wire depth for a source depth: 16 and 24 as they are; anything else 24 (Float32 carries 24 bits).
    private static func wireBits(_ bits: Int?) -> Int? {
        guard let b = bits else { return nil }
        return b == 16 ? 16 : 24
    }

    // MARK: - decisions

    private func decide(_ target: Fmt, name: String, tPlay: Date, how: String, manualStart: Bool, lossy: Bool = false) {
        trackTarget = target
        trackLossy = lossy
        state.trackRate = target.rate
        state.trackBits = target.bits
        state.lossless = !lossy && !trackAtmos
        state.source = how
        state.libraryCount = RateLibrary.shared.count
        let same = target == Fmt(rate: curRate, bits: curBits)
        // a manual start held its first sound while we waited: start the track over rather than send
        // the held audio late (nothing of it has been heard yet)
        let heldLong = manualStart && holdReached() && (reachedSeenAt.map { Date().timeIntervalSince($0) > 0.3 } ?? true)
        if same && !heldLong {
            elog("new track \(name): \(Int(target.rate)) Hz / \(target.bits)-bit (\(how)): no change")
            state.lastAction = "\(rateText(target.rate)) / \(target.bits)-bit, no switch"
            release("same format")
            publish()
            scheduleCheck(name)
            return
        }
        elog("new track \(name): \(Int(target.rate)) Hz / \(target.bits)-bit (\(how)): \(same ? "restarting it (held while waiting)" : "switching from \(Int(curRate)) Hz / \(curBits)-bit")")
        switchFormat(rate: target.rate, bits: target.bits, name: name, tPlay: tPlay)
    }

    /// The playing track went to another variant: switch the device now, without pausing or rewinding.
    /// The driver sends what it holds at the old format under the old header, marks the end and goes on
    /// at the new one.
    private func upgrade(to f: Fmt, why: String) {
        trackTarget = f
        guard f != Fmt(rate: curRate, bits: curBits) else { publish(); return }
        inSwitch = true
        defer { inSwitch = false }
        let t0 = Date()
        let from = "\(Int(curRate)) Hz / \(curBits)-bit"
        release("upgrade")
        // A stream's own variant upgrade (48 -> 96 kHz a few seconds in) has no rewind to hide a
        // lead-in in: it would add half a second of silence to a gap Music already makes. The
        // driver applies this to the next format change only (older drivers refuse it: ignored).
        ScreamDevice.setInt(device, ScreamDevice.leadOnce, 0)
        if f.bits != curBits { ScreamDevice.setInt(device, ScreamDevice.wireBits, f.bits) }
        if f.rate != curRate {
            ScreamDevice.setNominalRate(device, f.rate)
            let end = Date().addingTimeInterval(1)
            while Date() < end, ScreamDevice.nominalRate(device) != f.rate { Thread.sleep(forTimeInterval: 0.01) }
        }
        curRate = ScreamDevice.nominalRate(device) ?? f.rate
        curBits = f.bits
        formatChangedAt = Date()
        gridLast = nil
        gridBad = 0
        learnCleanFrames = 0
        state.switches += 1
        state.lastAction = "upgraded to \(rateText(curRate)) / \(curBits)-bit"
        state.source = "grid check"
        elog("  live switch \(state.switches): \(state.trackName) \(why) to \(Int(curRate)) Hz / \(curBits)-bit (from \(from)), no rewind; \(String(format: "%.3f", Date().timeIntervalSince(t0))) s")
        publish()
        scheduleCheck(state.trackName)
    }

    private func switchFormat(rate: Double, bits: Int, name: String, tPlay: Date) {
        inSwitch = true
        defer { inSwitch = false }
        let t0 = Date()
        let pauseTime = t0
        let wasPlaying = musicRunning() && scripts.playerState() == "playing"
        scripts.pause()

        // hold where the old track ended (armed boundary or gate), else find its end in what is still buffered
        let how: String
        if armed == .run { setHold(.boundary) }
        let reached = waitHold(1.0)
        how = reached ? "held at the boundary" : "no boundary found: cut at the pause"
        setHold(.drop)                       // the wrong-rate start and the pause fade go nowhere

        if bits != curBits { ScreamDevice.setInt(device, ScreamDevice.wireBits, bits) }
        if rate != curRate {
            ScreamDevice.setNominalRate(device, rate)
            let end = Date().addingTimeInterval(1)
            while Date() < end, ScreamDevice.nominalRate(device) != rate { Thread.sleep(forTimeInterval: 0.01) }
        }
        curRate = ScreamDevice.nominalRate(device) ?? rate
        curBits = bits
        formatChangedAt = Date()
        gridLast = nil
        gridBad = 0
        learnCleanFrames = 0

        // rewind to where the track began playing (measured to the pause; near the start = the start)
        let pos = scripts.position() ?? 0
        let played = pauseTime.timeIntervalSince(tPlay) + 0.05
        var startPos = pos - played - 0.1
        if startPos < 2 { startPos = 0.001 }    // not 0: Music takes "set to 0" for a no-op
        scripts.setPosition(startPos)
        // Keep dropping through the rewind and the play command. Music answers the seek with ~36 ms
        // of the track's start, then nothing for the ~0.4 s the play command takes: released before
        // play, that fragment went out, then a hole of zeros, then the track again (a stutter).
        if wasPlaying || playing {
            scripts.play()
            let end = Date().addingTimeInterval(0.5)
            while Date() < end, scripts.playerState() != "playing" { Thread.sleep(forTimeInterval: 0.02) }
            Thread.sleep(forTimeInterval: Self.rewindSettle)
        }
        setHold(.run)
        ownLinesUntil = max(ownLinesUntil ?? .distantPast, Date().addingTimeInterval(1))
        state.switches += 1
        state.lastAction = "switched to \(rateText(curRate)) / \(curBits)-bit"
        elog("  switch \(state.switches): \(how); device \(Int(curRate)) Hz, wire \(curBits)-bit; rewound to \(String(format: "%.3f", startPos)) (was \(String(format: "%.3f", pos))); done in \(String(format: "%.3f", Date().timeIntervalSince(t0))) s")
        publish()
        scheduleCheck(name)
        if wasPlaying || playing { confirmPlaying() }
    }

    /// Music can end up paused after our play (late notices of our own pause): play again, up to 3 times.
    private func confirmPlaying() {
        for attempt in 1...3 {
            Thread.sleep(forTimeInterval: 0.4)
            guard let st = scripts.playerState() else { return }
            if st == "playing" { return }
            elog("  Music is \(st) after the switch; play (\(attempt))")
            scripts.play()
        }
    }

    // MARK: - hold point

    private func setHold(_ h: ScreamDevice.Hold) {
        armed = h
        reachedSeenAt = nil
        armSerial += 1
        ScreamDevice.setInt(device, ScreamDevice.hold, h.rawValue)
    }

    private func holdReached() -> Bool { ScreamDevice.getInt(device, ScreamDevice.holdReached) == 1 }

    private func waitHold(_ seconds: Double) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            if holdReached() { return true }
            Thread.sleep(forTimeInterval: 0.01)
        }
        return holdReached()
    }

    private func release(_ why: String) {
        guard armed != .run else { return }
        elog("hold released (\(why))")
        setHold(.run)
    }

    /// Arms a boundary hold that is released again if no new track comes: unreached after `expiry`
    /// seconds, or reached (a silent passage, not a track change) with no new track within 1 s.
    private func arm(_ h: ScreamDevice.Hold, expiry: Double) {
        setHold(h)
        let serial = armSerial
        let since = Date()
        func check() {
            guard serial == armSerial, !inSwitch, armed == h else { return }
            if holdReached() {
                if reachedSeenAt == nil { reachedSeenAt = Date() }
                // not while a new track is waiting for its decoder line (up to startWait): that hold is
                // deliberate, and releasing it let the new track's first second out at the old rate
                if awaiting == nil, Date().timeIntervalSince(reachedSeenAt!) > 1.0, (lastNewTrackAt ?? .distantPast) < since {
                    release("reached but no new track: a silence inside the track")
                    return
                }
            } else if Date().timeIntervalSince(since) > expiry {
                release("armed \(Int(expiry)) s without a boundary")
                return
            }
            q.asyncAfter(deadline: .now() + 0.1) { check() }
        }
        q.asyncAfter(deadline: .now() + 0.1) { check() }
    }

    /// Music doesn't always post Playing (around a stream's switch to its lossless decoder it posts
    /// Paused and plays on): if sound reaches the gate, ask Music what it is doing.
    private func watchGate() {
        let serial = armSerial
        func check() {
            guard serial == armSerial, armed == .gate, !inSwitch, !stopped else { return }
            if holdReached(), musicRunning(), scripts.playerState() == "playing",
               let name = scripts.trackRate()?.name, !name.isEmpty {   // no name yet: its Playing notice is due
                playing = true
                if name == state.trackName {
                    ownLinesUntil = max(ownLinesUntil ?? .distantPast, Date().addingTimeInterval(2))
                    release("Music is playing \(name) without a Playing notice")
                } else {
                    elog("Music is playing \(name) without a Playing notice: a new track")
                    newTrack(name: name, id: Int64(truncatingIfNeeded: name.hashValue), at: Date())
                }
                return
            }
            q.asyncAfter(deadline: .now() + 0.15) { check() }
        }
        q.asyncAfter(deadline: .now() + 0.15) { check() }
    }

    private func scheduleArm(after seconds: Double) {
        let id = lastTrackID
        q.asyncAfter(deadline: .now() + seconds) { [weak self] in
            guard let self, !self.stopped, !self.inSwitch, self.playing, self.lastTrackID == id, self.armed == .run else { return }
            elog("boundary latch armed (1.5 s before the end)")
            self.arm(.boundary, expiry: 5)
        }
    }

    // MARK: - grid check

    /// Every 0.5 s: a lossless track at the device's format arrives on the wire's grid. Off the grid
    /// for 1 s (two windows) in a row, past the start variant (3 s after the track starts or after a
    /// seek), means the device format is wrong (a stream that went to its higher variant, or settled
    /// on another one than its pre-roll showed): move the device to a format the track's decoder
    /// showed, without a rewind. At most 3 tries per track, never back to one that failed.
    private func gridTick() {
        q.asyncAfter(deadline: .now() + Self.gridInterval) { [weak self] in
            guard let self, !self.stopped else { return }
            self.gridCheck()
            self.gridTick()
        }
    }

    private func gridCheck() {
        guard playing, !inSwitch, armed == .run, let st = DriverStatus(ScreamDevice.get(device, ScreamDevice.status)), st.running else {
            gridLast = nil
            return
        }
        defer { gridLast = (st.runFrames, st.runOffGridSamples) }
        guard let last = gridLast, st.runFrames > last.frames, st.runOffGridSamples >= last.offGrid else { return }
        let settled = max(lastNewTrackAt ?? .distantPast, lastSeekAt ?? .distantPast)
        guard Date().timeIntervalSince(settled) > 3, Date().timeIntervalSince(formatChangedAt) > 1 else { return }
        let fDelta = st.runFrames - last.frames
        let oDelta = st.runOffGridSamples - last.offGrid
        if oDelta == 0 { learnCleanFrames += fDelta }
        // lossy (AAC) or Atmos content is never on the grid: don't chase it
        if trackLossy || trackAtmos { return }
        let ratio = Double(oDelta) / Double(fDelta * 2)
        if ratio < 0.3 {
            gridBad = 0
            return
        }
        gridBad += 1
        guard gridBad >= 2 else { return }
        gridBad = 0
        let cur = Fmt(rate: curRate, bits: curBits)
        // all decoder formats for this track are candidates; the track's own (reliable) first
        let candidates = trackFormats.reversed() + maybeFormats.reversed()
        // never back to a format this track already failed the check at
        gridTried.append(cur)
        guard gridFixes < 3, let f = candidates.first(where: { !gridTried.contains($0) }) else {
            if gridFixes < 3 {
                gridFixes = 3
                forgetLibraryEntry(if: libraryStart || libraryCandidate)
                elog("grid check: \(state.trackName) is off the \(curBits)-bit grid (\(Int(ratio * 100))%) and its decoder showed no other format: not bit-perfect (lossy, or processed in Music)")
            }
            return
        }
        gridFixes += 1
        forgetLibraryEntry(if: libraryStart)
        elog("grid check: \(state.trackName) is off the \(curBits)-bit grid at \(Int(curRate)) Hz (\(Int(ratio * 100))% of samples); its decoder showed \(Int(f.rate)) Hz / \(f.bits)-bit (try \(gridFixes))")
        trackTarget = f
        state.trackRate = f.rate
        state.trackBits = f.bits
        upgrade(to: f, why: "corrected by the grid check")
    }

    // MARK: - rate library

    /// The Rate Library's format for this track did not hold up on the grid: drop it so the next play
    /// is decided on fresh evidence (the track is learned again if it ends up clean).
    private func forgetLibraryEntry(if wrong: Bool) {
        guard wrong, let key = currentKey, RateLibrary.shared.lookup(key) != nil else { return }
        RateLibrary.shared.remove(key)
        libraryStart = false
        libraryCandidate = false
        state.libraryCount = RateLibrary.shared.count
        elog("rate library: dropped \(state.trackName) (its remembered format did not match the audio)")
    }

    /// Records the outgoing track's outcome (a track that played clean ≥ 10 s after its last format
    /// change counts as confirmed; a lossy/Atmos one is remembered as lossy so nothing chases it).
    private func finalizeLearning() {
        guard let key = currentKey else { return }
        currentKey = nil
        let cleanSeconds = learnCleanFrames > 0 && curRate > 0 ? Double(learnCleanFrames) / curRate : 0
        // lossy / Atmos plays teach nothing (never on the grid, nothing to reuse)
        if !trackLossy && !trackAtmos && cleanSeconds >= 10 {
            RateLibrary.shared.learn(key, rate: curRate, bits: curBits)
            elog("rate library: \(state.trackName) learned \(Int(curRate)) Hz / \(curBits)-bit (\(Int(cleanSeconds)) s clean)")
        }
        learnCleanFrames = 0
    }

    /// Logs the driver's bit-perfect check for the current run a few seconds into a track.
    private func scheduleCheck(_ name: String) {
        for delay in [4.0, 20.0] {
            q.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self, !self.stopped, self.state.trackName == name,
                      let st = DriverStatus(ScreamDevice.get(self.device, ScreamDevice.status)), st.running else { return }
                let samples = st.runFrames * 2
                let raw = ScreamDevice.get(self.device, ScreamDevice.status) as? [String: Any]
                let skipped = raw?["musicSkippedCycles"] as? Int ?? -1
                let gain = raw?["runGain"] as? Double ?? 0, dev = raw?["runMeanDeviationLSB"] as? Double ?? 0
                let vol = self.musicRunning() ? self.scripts.volume() : nil
                let eq = self.musicRunning() ? self.scripts.eqEnabled() : nil
                elog("check \(name) +\(Int(delay)) s: \(st.rate) Hz / \(st.wireBits)-bit, \(st.runFrames) frames, \(st.runOffGridSamples) of \(samples) samples off the grid\(st.runOffGridSamples == 0 && samples > 0 ? " (bit-perfect)" : ""), underruns \(st.underruns), cycles without Music \(skipped); fit gain \(String(format: "%.9f", gain)), mean deviation \(String(format: "%.3f", dev)) LSB; Music volume \(vol.map(String.init) ?? "?"), EQ \(eq.map { $0 ? "on" : "off" } ?? "?")")
            }
        }
    }

    private func publish() {
        let s = state
        DispatchQueue.main.async { [weak self] in self?.onChange?(s) }
    }

    private func rateText(_ r: Double) -> String {
        r.truncatingRemainder(dividingBy: 1000) == 0 ? "\(Int(r / 1000)) kHz" : String(format: "%.1f kHz", r / 1000)
    }
}
