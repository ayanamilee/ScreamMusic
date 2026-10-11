//
//  EngineLog.swift
//  ScreamMusic
//
//  ~/Library/Logs/ScreamMusic.log: one "[seconds] message" line per event, seconds since the app
//  started. The previous two runs are kept as .1 and .2.
//

import Foundation

final class EngineLog: @unchecked Sendable {
    static let shared = EngineLog()

    private var handle: FileHandle?
    private let t0 = Date()
    private let lock = NSLock()

    private init() {
        let fm = FileManager.default
        let logs = fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs")
        let url = logs.appendingPathComponent("ScreamMusic.log")
        let one = logs.appendingPathComponent("ScreamMusic.1.log"), two = logs.appendingPathComponent("ScreamMusic.2.log")
        if fm.fileExists(atPath: url.path) {
            try? fm.removeItem(at: two)
            try? fm.moveItem(at: one, to: two)
            try? fm.moveItem(at: url, to: one)
        }
        fm.createFile(atPath: url.path, contents: nil)
        handle = try? FileHandle(forWritingTo: url)
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS ZZZZZ"
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
        write("log started \(f.string(from: t0)); ScreamMusic \(version); macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
    }

    func write(_ s: String) {
        let line = String(format: "[%8.3f] ", Date().timeIntervalSince(t0)) + s + "\n"
        lock.lock()
        handle?.write(line.data(using: .utf8)!)
        lock.unlock()
    }
}

func elog(_ s: String) { EngineLog.shared.write(s) }
