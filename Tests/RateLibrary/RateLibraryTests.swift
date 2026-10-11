import Foundation
import Testing
@testable import ScreamMusicKit

@Suite("RateLibrary")
struct RateLibraryTests {

    private func fresh(_ name: String = "RateLibraryTests") -> RateLibrary {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("ScreamMusicTests-\(name)-\(UUID().uuidString).json")
        return RateLibrary(url: url)
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
        lib.learn(key, rate: 96000, bits: 24, lossy: false)
        let f = lib.lookup(key)
        #expect(f?.rate == 96000 && f?.bits == 24 && f?.lossy == false && f?.plays == 1)
        lib.learn(key, rate: 48000, bits: 24, lossy: false)   // corrected by a later play
        let g = lib.lookup(key)
        #expect(g?.rate == 48000 && g?.plays == 2)
    }

    @Test func persistence() {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("ScreamMusicTests-persist-\(UUID().uuidString).json")
        let lib = RateLibrary(url: url)
        lib.learn("t1", rate: 44100, bits: 16, lossy: false)
        let again = RateLibrary(url: url)
        #expect(again.lookup("t1")?.bits == 16)
    }

    @Test func albumHint() {
        let lib = fresh()
        let album = RateLibrary.key(name: "", artist: "Artist", album: "Album")
        lib.learn(album + " — one", rate: 96000, bits: 24, lossy: false)
        lib.learn(album + " — two", rate: 96000, bits: 24, lossy: false)
        lib.learn(album + " — three", rate: 44100, bits: 16, lossy: false)
        #expect(lib.albumHint(album)?.rate == 96000)
        #expect(lib.albumHint("nothing") == nil)
    }

    @Test func lossyEntriesAreNotAlbumHints() {
        let lib = fresh()
        let album = RateLibrary.key(name: "", artist: "A", album: "LossyAlbum")
        lib.learn(album + " — t", rate: 44100, bits: 24, lossy: true)
        #expect(lib.albumHint(album) == nil)
    }

    @Test func trim() {
        let lib = fresh()
        for i in 0..<(RateLibrary.maxEntries + 10) {
            lib.learn("track \(i)", rate: 44100, bits: 16, lossy: false)
        }
        #expect(lib.count == RateLibrary.maxEntries)
        #expect(lib.lookup("track 0") == nil)   // oldest evicted
        #expect(lib.lookup("track \(RateLibrary.maxEntries + 9)") != nil)
    }

    @Test func clear() {
        let lib = fresh()
        lib.learn("t", rate: 44100, bits: 16, lossy: false)
        lib.removeAll()
        #expect(lib.count == 0)
    }
}
