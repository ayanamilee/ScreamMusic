//
//  ScreamDevice.swift
//  ScreamMusic
//
//  Client-side access to the ScreamMusic device: finding it, its sample rate and the driver's
//  custom properties (DESIGN.md §4.4).
//

import CoreAudio
import Foundation

enum ScreamDevice {

    static let uid = "ScreamMusic_UID"

    // custom properties, as in Driver/ScreamMusicDriver.c
    static let destination: AudioObjectPropertySelector = 0x534D_6473 // 'SMds'
    static let status: AudioObjectPropertySelector = 0x534D_7374      // 'SMst'
    static let endOfTrack: AudioObjectPropertySelector = 0x534D_656F  // 'SMeo'
    static let sendDelay: AudioObjectPropertySelector = 0x534D_646C   // 'SMdl'
    static let leadIn: AudioObjectPropertySelector = 0x534D_6C69      // 'SMli'
    static let leadOnce: AudioObjectPropertySelector = 0x534D_6C6F    // 'SMlo'
    static let loopback: AudioObjectPropertySelector = 0x534D_6C62    // 'SMlb'
    static let wireBits: AudioObjectPropertySelector = 0x534D_6264    // 'SMbd'

    static let rates: [Double] = [44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000]

    /// The device, if the HAL has loaded the driver.
    static func find() -> AudioObjectID? {
        var addr = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var uidRef = uid as CFString
        var id = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let st = withUnsafeMutablePointer(to: &uidRef) { q in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, UInt32(MemoryLayout<CFString>.size), q, &size, &id)
        }
        return st == noErr && id != kAudioObjectUnknown ? id : nil
    }

    static func address(_ selector: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    static func get(_ device: AudioObjectID, _ selector: AudioObjectPropertySelector) -> CFPropertyList? {
        var addr = address(selector)
        var ref: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &ref) == noErr else { return nil }
        return ref?.takeRetainedValue()
    }

    @discardableResult
    static func set(_ device: AudioObjectID, _ selector: AudioObjectPropertySelector, _ value: CFPropertyList) -> OSStatus {
        var addr = address(selector)
        // the HAL takes a pointer to the CFPropertyListRef
        let ref: CFPropertyList = value
        return withUnsafePointer(to: ref) { p in
            AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<CFPropertyList>.size), p)
        }
    }

    static func nominalRate(_ device: AudioObjectID) -> Double? {
        var addr = address(kAudioDevicePropertyNominalSampleRate)
        var rate = Float64(0)
        var size = UInt32(MemoryLayout<Float64>.size)
        return AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &rate) == noErr ? rate : nil
    }

    @discardableResult
    static func setNominalRate(_ device: AudioObjectID, _ rate: Double) -> OSStatus {
        var addr = address(kAudioDevicePropertyNominalSampleRate)
        var r = Float64(rate)
        return AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<Float64>.size), &r)
    }

    /// The process that has the device in hog mode, or nil.
    static func hogOwner(_ device: AudioObjectID) -> pid_t? {
        var addr = address(kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeOutput)
        var pid = pid_t(-1)
        var size = UInt32(MemoryLayout<pid_t>.size)
        guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &pid) == noErr, pid > 0 else { return nil }
        return pid
    }
}

struct Destination: Equatable {
    var ip = "127.0.0.1"
    var port = 4011
    var header = "extended"

    init() {}

    init?(_ plist: CFPropertyList?) {
        guard let d = plist as? [String: Any] else { return nil }
        ip = d["ip"] as? String ?? ip
        port = d["port"] as? Int ?? port
        header = d["header"] as? String ?? header
    }

    var plist: CFDictionary { ["ip": ip, "port": port, "header": header] as CFDictionary }
}

struct DriverStatus {
    struct Client: Hashable { let pid: Int; let bundleID: String }

    var running = false
    var rate = 0
    var physicalFormat = ""
    var wireBits = 0
    var packets = 0
    var sendErrors = 0
    var lastErrno = 0
    var underruns = 0
    var overflowBytes = 0
    var offGridSamples = 0
    var endOfTrackMarkers = 0
    var runFrames = 0
    var runOffGridSamples = 0
    var othersFrames = 0
    var musicPID = 0
    var holdMode = 0
    var holdReached = false
    /// Silence before a new wire format, ms; nil: the driver predates it (0.4.5).
    var leadInMs: Int?
    /// Apple Music goes to the ScreamMusic AM input instead of the network; nil: the driver predates it (0.5.0).
    var loopback: Bool?
    var loopFrames = 0
    var loopUnderruns = 0
    var clients: [Client] = []

    init() {}

    init?(_ plist: CFPropertyList?) {
        guard let d = plist as? [String: Any] else { return nil }
        running = d["running"] as? Bool ?? false
        rate = d["rate"] as? Int ?? 0
        physicalFormat = d["physicalFormat"] as? String ?? ""
        wireBits = d["wireBits"] as? Int ?? 0
        packets = d["packets"] as? Int ?? 0
        sendErrors = d["sendErrors"] as? Int ?? 0
        lastErrno = d["lastErrno"] as? Int ?? 0
        underruns = d["underruns"] as? Int ?? 0
        overflowBytes = d["overflowBytes"] as? Int ?? 0
        offGridSamples = d["offGridSamples"] as? Int ?? 0
        endOfTrackMarkers = d["endOfTrackMarkers"] as? Int ?? 0
        runFrames = d["runFrames"] as? Int ?? 0
        runOffGridSamples = d["runOffGridSamples"] as? Int ?? 0
        othersFrames = d["othersFrames"] as? Int ?? 0
        musicPID = d["musicPID"] as? Int ?? 0
        holdMode = d["holdMode"] as? Int ?? 0
        holdReached = d["holdReached"] as? Bool ?? false
        leadInMs = d["leadInMs"] as? Int
        loopback = (d["loopback"] as? Int).map { $0 != 0 }
        loopFrames = d["loopFrames"] as? Int ?? 0
        loopUnderruns = d["loopUnderruns"] as? Int ?? 0
        clients = (d["clients"] as? [[String: Any]] ?? []).map { Client(pid: $0["pid"] as? Int ?? 0, bundleID: $0["bundleID"] as? String ?? "") }
    }
}

// MARK: - System devices

enum SystemAudio {

    static func devices() -> [AudioObjectID] {
        var addr = ScreamDevice.address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }

    static func string(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String {
        var addr = ScreamDevice.address(sel)
        var v: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(obj, &addr, 0, nil, &size, &v) == noErr else { return "" }
        return (v?.takeRetainedValue() as String?) ?? ""
    }

    static func uid(_ d: AudioObjectID) -> String { string(d, kAudioDevicePropertyDeviceUID) }
    static func name(_ d: AudioObjectID) -> String { string(d, kAudioObjectPropertyName) }

    static func device(uid: String) -> AudioObjectID? { devices().first { self.uid($0) == uid } }

    static func hasOutput(_ d: AudioObjectID) -> Bool {
        var addr = ScreamDevice.address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)
        var size: UInt32 = 0
        return AudioObjectGetPropertyDataSize(d, &addr, 0, nil, &size) == noErr && size > 0
    }

    static func transport(_ d: AudioObjectID) -> UInt32 {
        var addr = ScreamDevice.address(kAudioDevicePropertyTransportType)
        var t: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        AudioObjectGetPropertyData(d, &addr, 0, nil, &size, &t)
        return t
    }

    /// Outputs a user can send other apps to: not ScreamMusic, not a process's private aggregate.
    static func outputDevices() -> [AudioObjectID] {
        devices().filter { hasOutput($0) && uid($0) != ScreamDevice.uid && !name($0).hasPrefix("CADefaultDeviceAggregate") }
    }

    static func builtInOutput() -> AudioObjectID? {
        outputDevices().first { transport($0) == kAudioDeviceTransportTypeBuiltIn }
    }

    private static func systemDevice(_ sel: AudioObjectPropertySelector) -> AudioObjectID? {
        var addr = ScreamDevice.address(sel)
        var d = AudioObjectID(0)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &d) == noErr, d != 0 else { return nil }
        return d
    }

    @discardableResult
    private static func setSystemDevice(_ sel: AudioObjectPropertySelector, _ d: AudioObjectID) -> OSStatus {
        var addr = ScreamDevice.address(sel)
        var v = d
        return AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, UInt32(MemoryLayout<AudioObjectID>.size), &v)
    }

    static func defaultOutput() -> AudioObjectID? { systemDevice(kAudioHardwarePropertyDefaultOutputDevice) }
    static func alertOutput() -> AudioObjectID? { systemDevice(kAudioHardwarePropertyDefaultSystemOutputDevice) }
    @discardableResult static func setDefaultOutput(_ d: AudioObjectID) -> OSStatus { setSystemDevice(kAudioHardwarePropertyDefaultOutputDevice, d) }
    @discardableResult static func setAlertOutput(_ d: AudioObjectID) -> OSStatus { setSystemDevice(kAudioHardwarePropertyDefaultSystemOutputDevice, d) }
}

extension ScreamDevice {
    static let musicPID: AudioObjectPropertySelector = 0x534D_6D78    // 'SMmx'
    static let hold: AudioObjectPropertySelector = 0x534D_6864        // 'SMhd'
    static let holdReached: AudioObjectPropertySelector = 0x534D_6872 // 'SMhr'
    static let owner: AudioObjectPropertySelector = 0x534D_6163       // 'SMac'

    enum Hold: Int { case run = 0, gate = 1, boundary = 2, drop = 3 }

    static func getInt(_ d: AudioObjectID, _ sel: AudioObjectPropertySelector) -> Int? { get(d, sel) as? Int }
    @discardableResult static func setInt(_ d: AudioObjectID, _ sel: AudioObjectPropertySelector, _ v: Int) -> OSStatus { set(d, sel, v as CFNumber) }
}
