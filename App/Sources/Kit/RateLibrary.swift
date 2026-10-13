//
//  RateLibrary.swift
//  ScreamMusic
//
//  Remembers the format each track actually played at (the idea from Lysoniq Switcher's Rate
//  Library): a track that has played once can have its device set at the gate on a manual start,
//  instead of holding for its decoder line. It is only ever a hint: a track's own pre-roll line,
//  decoder line and the grid check outrank it, and an entry the grid check had to correct is dropped
//  and learned again.
//
//  Keys: the track's persistent ID when Music has one (local and library tracks), else the
//  normalised "artist — album — name" (see key(name:artist:album:)).
//
//  Thread-safe: the follower's queue reads and writes, the UI clears. Saving is asynchronous.
//

import Foundation

public struct LearnedFormat: Codable, Equatable, Sendable {
    public var rate: Double
    public var bits: Int
    /// Only read from files written by 0.4.0/0.4.1, which also remembered lossy tracks; those
    /// entries are dropped on load (a lossy mark kept itself alive across plays).
    public var lossy: Bool
    public var confirmedAt: Date
    public var plays: Int

    public init(rate: Double, bits: Int, lossy: Bool = false, confirmedAt: Date, plays: Int) {
        self.rate = rate
        self.bits = bits
        self.lossy = lossy
        self.confirmedAt = confirmedAt
        self.plays = plays
    }
}

public final class RateLibrary: @unchecked Sendable {

    public static let maxEntries = 5000

    private let lock = NSLock()
    private var entries: [String: LearnedFormat] = [:]
    private var errorHandler: (String) -> Void = { _ in }
    private let url: URL
    private let saveQueue = DispatchQueue(label: "ScreamMusic.RateLibrary.save", qos: .utility)

    /// The shared instance stores to ~/Library/Application Support/ScreamMusic/RateLibrary.json.
    public static let shared = RateLibrary()

    public init(url: URL? = nil) {
        if let url {
            self.url = url
        } else {
            let dir = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/ScreamMusic")
            self.url = dir.appendingPathComponent("RateLibrary.json")
        }
        load()
    }

    /// Save failures land here (the app wires it to its log). Default: silent.
    public func setErrorHandler(_ handler: @escaping (String) -> Void) {
        lock.lock(); defer { lock.unlock() }
        errorHandler = handler
    }

    public var count: Int {
        lock.lock(); defer { lock.unlock() }
        return entries.count
    }

    // MARK: - keys

    /// Normalised "artist — album — name" (or any subset that is non-empty). Empty if nothing is known.
    public static func key(name: String, artist: String = "", album: String = "") -> String {
        func clean(_ s: String) -> String {
            s.trimmingCharacters(in: .whitespacesAndNewlines)
                .replacingOccurrences(of: #"\s+"#, with: " ", options: .regularExpression)
                .lowercased()
        }
        let parts = [artist, album, name].map(clean).filter { !$0.isEmpty }
        return parts.joined(separator: " — ")
    }

    // MARK: - lookup / learn / forget

    public func lookup(_ trackKey: String) -> LearnedFormat? {
        lock.lock(); defer { lock.unlock() }
        return entries[trackKey]
    }

    /// Records the outcome of a play. Returns the entry now stored.
    @discardableResult
    public func learn(_ trackKey: String, rate: Double, bits: Int) -> LearnedFormat {
        lock.lock()
        var f = entries[trackKey] ?? LearnedFormat(rate: rate, bits: bits, confirmedAt: .distantPast, plays: 0)
        f.rate = rate
        f.bits = bits
        f.lossy = false
        f.confirmedAt = Date()
        f.plays += 1
        entries[trackKey] = f
        trim()
        let snapshot = entries
        lock.unlock()
        save(snapshot)
        return f
    }

    /// Drops one entry (the grid check showed it was wrong).
    public func remove(_ trackKey: String) {
        lock.lock()
        let had = entries.removeValue(forKey: trackKey) != nil
        let snapshot = entries
        lock.unlock()
        if had { save(snapshot) }
    }

    public func removeAll() {
        lock.lock()
        entries.removeAll()
        lock.unlock()
        save([:])
    }

    /// Blocks until pending saves have reached the disk (quit, tests).
    public func flush() {
        saveQueue.sync {}
    }

    // MARK: - persistence

    private func load() {
        guard let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([String: LearnedFormat].self, from: data)
        else { return }
        lock.lock(); defer { lock.unlock() }
        entries = decoded.filter { !$0.value.lossy }
    }

    private func save(_ snapshot: [String: LearnedFormat]) {
        saveQueue.async { [self] in
            do {
                try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                let data = try JSONEncoder().encode(snapshot)
                let tmp = url.appendingPathExtension("tmp")
                try data.write(to: tmp, options: .atomic)
                _ = try FileManager.default.replaceItemAt(url, withItemAt: tmp)
            } catch {
                lock.lock()
                let report = errorHandler
                lock.unlock()
                report("rate library: could not save: \(error.localizedDescription)")
            }
        }
    }

    /// Caller holds the lock.
    private func trim() {
        guard entries.count > Self.maxEntries else { return }
        let oldestFirst = entries.sorted { $0.value.confirmedAt < $1.value.confirmedAt }
        for (key, _) in oldestFirst.prefix(entries.count - Self.maxEntries) {
            entries.removeValue(forKey: key)
        }
    }
}
