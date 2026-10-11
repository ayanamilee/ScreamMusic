//
//  RateLibrary.swift
//  ScreamMusic
//
//  Remembers the format each track actually played at (the idea from Lysoniq Switcher's Rate
//  Library): a track that has played once gets its device set before the music starts next time,
//  instead of holding at the gate for its decoder log. Correction via the grid check updates the
//  entry; a track that stayed off-grid with no candidates is remembered as lossy so nothing chases
//  it again.
//
//  Keys: the track's persistent ID when Music has one (local and library tracks), else the
//  normalised "artist — album — name". Album-level hints use the "artist — album" prefix.
//

import Foundation

public struct LearnedFormat: Codable, Equatable {
    public var rate: Double
    public var bits: Int
    public var lossy: Bool
    public var confirmedAt: Date
    public var plays: Int

    public init(rate: Double, bits: Int, lossy: Bool, confirmedAt: Date, plays: Int) {
        self.rate = rate
        self.bits = bits
        self.lossy = lossy
        self.confirmedAt = confirmedAt
        self.plays = plays
    }
}

/// Not thread-safe on its own; the app uses it only from the follower's serial queue.
public final class RateLibrary: @unchecked Sendable {

    public static let maxEntries = 5000

    private var entries: [String: LearnedFormat] = [:]
    private let url: URL

    /// Save failures land here (the app wires it to its log). Default: silent.
    public var onError: (String) -> Void = { _ in }

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

    public var count: Int { entries.count }

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

    public static func albumKey(artist: String, album: String) -> String { key(name: "", artist: artist, album: album) }

    // MARK: - lookup

    public func lookup(_ trackKey: String) -> LearnedFormat? { entries[trackKey] }

    /// The most common format among this album's other tracks, if any. A hint only: the grid check
    /// still verifies.
    public func albumHint(_ album: String) -> LearnedFormat? {
        guard !album.isEmpty else { return nil }
        var counts: [LearnedFormatKey: (format: LearnedFormat, count: Int)] = [:]
        for (key, value) in entries where key.hasPrefix(album + " — ") && !value.lossy {
            let k = LearnedFormatKey(rate: value.rate, bits: value.bits)
            let c = counts[k] ?? (value, 0)
            counts[k] = (c.format, c.count + 1)
        }
        return counts.values.max(by: { $0.count < $1.count })?.format
    }

    private struct LearnedFormatKey: Hashable { let rate: Double; let bits: Int }

    // MARK: - learn

    /// Records the outcome of a play. Returns the entry now stored.
    @discardableResult
    public func learn(_ trackKey: String, rate: Double, bits: Int, lossy: Bool) -> LearnedFormat {
        var f = entries[trackKey] ?? LearnedFormat(rate: rate, bits: bits, lossy: lossy, confirmedAt: .distantPast, plays: 0)
        f.rate = rate
        f.bits = bits
        f.lossy = lossy
        f.confirmedAt = Date()
        f.plays += 1
        entries[trackKey] = f
        trim()
        save()
        return f
    }

    public func removeAll() {
        entries.removeAll()
        save()
    }

    // MARK: - persistence

    private func load() {
        guard let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode([String: LearnedFormat].self, from: data)
        else { return }
        entries = decoded
    }

    private func save() {
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(entries)
            let tmp = url.appendingPathExtension("tmp")
            try data.write(to: tmp, options: .atomic)
            _ = try FileManager.default.replaceItemAt(url, withItemAt: tmp)
        } catch {
            onError("rate library: could not save: \(error.localizedDescription)")
        }
    }

    private func trim() {
        guard entries.count > Self.maxEntries else { return }
        let oldestFirst = entries.sorted { $0.value.confirmedAt < $1.value.confirmedAt }
        for (key, _) in oldestFirst.prefix(entries.count - Self.maxEntries) {
            entries.removeValue(forKey: key)
        }
    }
}
