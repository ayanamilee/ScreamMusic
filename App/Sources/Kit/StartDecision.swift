//
//  StartDecision.swift
//  ScreamMusic
//
//  What format a new track starts at. The Rate Library is a hint for when nothing fresher is
//  known: a track's own pre-roll line (a natural transition) outranks it, and then the library's
//  format is only a candidate for the grid check.
//

import Foundation

public struct WireFormat: Equatable {
    public var rate: Double
    public var bits: Int

    public init(rate: Double, bits: Int) {
        self.rate = rate
        self.bits = bits
    }

    public func higher(than o: WireFormat) -> Bool { rate > o.rate || (rate == o.rate && bits > o.bits) }

    public static func best(_ a: WireFormat?, _ b: WireFormat?) -> WireFormat? {
        guard let a else { return b }
        guard let b else { return a }
        return b.higher(than: a) ? b : a
    }
}

public struct StartChoice: Equatable {
    public enum Source: String { case preRoll = "pre-roll", library = "rate library" }

    public var target: WireFormat
    public var source: Source
    /// The format to switch to before the first sound is heard (a manual start has no pre-roll).
    public var manualStart: Bool
    /// A format the grid check may fall back on if the samples say `target` is wrong.
    public var candidate: WireFormat?
}

/// nil: nothing is known yet; hold and wait for the track's own decoder line.
public func chooseStart(preRoll: WireFormat?, scriptRate: Double?, learned: LearnedFormat?, followDepth: Bool) -> StartChoice? {
    let remembered = learned.flatMap { $0.lossy ? nil : WireFormat(rate: $0.rate, bits: followDepth ? $0.bits : 24) }
    if let preRoll {
        let target = WireFormat.best(preRoll, scriptRate.map { WireFormat(rate: $0, bits: preRoll.bits) })!
        return StartChoice(target: target, source: .preRoll, manualStart: false,
                           candidate: remembered.flatMap { $0 == target ? nil : $0 })
    }
    if let remembered {
        return StartChoice(target: remembered, source: .library, manualStart: true, candidate: nil)
    }
    return nil
}
