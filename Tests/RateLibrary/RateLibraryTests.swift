import Foundation
import Testing
@testable import ScreamMusicKit

@Suite("RateLibrary")
struct RateLibraryTests {

    private func tempURL(_ name: String) -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("ScreamMusicTests-\(name)-\(UUID().uuidString).json")
    }

    private func fresh(_ name: String = "RateLibraryTests") -> RateLibrary {
        RateLibrary(url: tempURL(name))
    }

    @Test func keyNormalisation() {
        #expect(RateLibrary.key(name: "Song") == "song")
        #expect(RateLibrary.key(name: " Song  Name ", artist: "An Artist", album: "An Album") == "an artist — an album — song name")
        #expect(RateLibrary.key(name: "") == "")
    }

    @Test func learnAndLookup() {
        let lib = fresh()
        let key = RateLibrary.key(name: "Track", artist: "A", album: "B")
        #expect(lib.lookup(key) == nil)
        lib.learn(key, rate: 96000, bits: 24)
        let f = lib.lookup(key)
        #expect(f?.rate == 96000 && f?.bits == 24 && f?.lossy == false && f?.plays == 1)
        lib.learn(key, rate: 48000, bits: 24)   // corrected by a later play
        let g = lib.lookup(key)
        #expect(g?.rate == 48000 && g?.plays == 2)
    }

    @Test func persistence() {
        let url = tempURL("persist")
        let lib = RateLibrary(url: url)
        lib.learn("t1", rate: 44100, bits: 16)
        lib.flush()
        let again = RateLibrary(url: url)
        #expect(again.lookup("t1")?.bits == 16)
    }

    @Test func removeForgetsOneEntry() {
        let url = tempURL("remove")
        let lib = RateLibrary(url: url)
        lib.learn("a", rate: 96000, bits: 24)
        lib.learn("b", rate: 48000, bits: 24)
        lib.remove("a")
        lib.flush()
        #expect(lib.lookup("a") == nil && lib.lookup("b") != nil)
        #expect(RateLibrary(url: url).lookup("a") == nil)
    }

    /// 0.4.0/0.4.1 also stored lossy tracks; a file from them must not bring those back.
    @Test func lossyEntriesFromOldFilesAreDropped() throws {
        let url = tempURL("legacy")
        let old: [String: LearnedFormat] = [
            "aac": LearnedFormat(rate: 48000, bits: 24, lossy: true, confirmedAt: Date(), plays: 3),
            "alac": LearnedFormat(rate: 96000, bits: 24, lossy: false, confirmedAt: Date(), plays: 1),
        ]
        try JSONEncoder().encode(old).write(to: url)
        let lib = RateLibrary(url: url)
        #expect(lib.lookup("aac") == nil)
        #expect(lib.lookup("alac")?.rate == 96000)
        #expect(lib.count == 1)
    }

    @Test func trim() {
        let lib = fresh()
        for i in 0..<(RateLibrary.maxEntries + 10) {
            lib.learn("track \(i)", rate: 44100, bits: 16)
        }
        #expect(lib.count == RateLibrary.maxEntries)
        #expect(lib.lookup("track 0") == nil)   // oldest evicted
        #expect(lib.lookup("track \(RateLibrary.maxEntries + 9)") != nil)
    }

    @Test func clear() {
        let lib = fresh()
        lib.learn("t", rate: 44100, bits: 16)
        lib.removeAll()
        #expect(lib.count == 0)
    }

    /// The follower's queue learns while the UI clears; this must not crash or corrupt.
    @Test func concurrentUseIsSafe() async {
        let lib = fresh()
        await withTaskGroup(of: Void.self) { group in
            for t in 0..<4 {
                group.addTask {
                    for i in 0..<300 {
                        lib.learn("t\(t)-\(i % 50)", rate: 96000, bits: 24)
                        _ = lib.lookup("t\(t)-\(i % 50)")
                        if i % 97 == 0 { lib.remove("t\(t)-0") }
                    }
                }
            }
            group.addTask {
                for _ in 0..<20 { lib.removeAll(); _ = lib.count }
            }
        }
        lib.flush()
        #expect(lib.count <= 200)
    }
}

@Suite("StartDecision")
struct StartDecisionTests {
    private let hires = WireFormat(rate: 96000, bits: 24)
    private let cd = WireFormat(rate: 44100, bits: 16)
    private func learned(_ rate: Double, _ bits: Int, lossy: Bool = false) -> LearnedFormat {
        LearnedFormat(rate: rate, bits: bits, lossy: lossy, confirmedAt: Date(), plays: 1)
    }

    @Test func nothingKnownWaits() {
        #expect(chooseStart(preRoll: nil, scriptRate: 96000, learned: nil, followDepth: true) == nil)
    }

    @Test func manualStartUsesLibrary() {
        let c = chooseStart(preRoll: nil, scriptRate: nil, learned: learned(96000, 24), followDepth: true)
        #expect(c?.target == hires && c?.source == .library && c?.manualStart == true && c?.candidate == nil)
    }

    /// A gapless transition has the track's real format in its pre-roll: it wins over a stale entry,
    /// which stays a candidate for the grid check.
    @Test func preRollOutranksLibrary() {
        let c = chooseStart(preRoll: cd, scriptRate: nil, learned: learned(96000, 24), followDepth: true)
        #expect(c?.target == cd && c?.source == .preRoll && c?.manualStart == false)
        #expect(c?.candidate == hires)
    }

    @Test func agreeingLibraryAddsNoCandidate() {
        let c = chooseStart(preRoll: hires, scriptRate: nil, learned: learned(96000, 24), followDepth: true)
        #expect(c?.candidate == nil)
    }

    @Test func preRollTakesMusicsHigherRate() {
        let c = chooseStart(preRoll: cd, scriptRate: 96000, learned: nil, followDepth: true)
        #expect(c?.target == WireFormat(rate: 96000, bits: 16))
    }

    @Test func lossyEntryIsIgnored() {
        #expect(chooseStart(preRoll: nil, scriptRate: nil, learned: learned(48000, 24, lossy: true), followDepth: true) == nil)
    }

    @Test func depthFollowingOffUses24() {
        let c = chooseStart(preRoll: nil, scriptRate: nil, learned: learned(44100, 16), followDepth: false)
        #expect(c?.target == WireFormat(rate: 44100, bits: 24))
        let on = chooseStart(preRoll: nil, scriptRate: nil, learned: learned(44100, 16), followDepth: true)
        #expect(on?.target == cd)
    }
}
