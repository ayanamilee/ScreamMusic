//
//  OthersPlayer.swift
//  ScreamMusic
//
//  Apple Music mode: what every app but Music played into ScreamMusic (the driver moves it to the
//  device's input stream) plays on another output. Not bit-perfect and not meant to be:
//  AVAudioEngine converts ScreamMusic's rate to the output's, and the ring between the two clocks
//  is kept short by dropping when it grows and padding with silence when it runs dry.
//  Needs Microphone permission (it reads a device's input).
//

import AVFoundation
import CoreAudio
import Foundation

/// Single-producer single-consumer ring of interleaved stereo Float32 frames.
final class FrameRing: @unchecked Sendable {
    private let capacity: Int
    private let buf: UnsafeMutablePointer<Float>
    private let head = UnsafeMutablePointer<Int>.allocate(capacity: 1)   // frames written
    private let tail = UnsafeMutablePointer<Int>.allocate(capacity: 1)   // frames read

    init(frames: Int) {
        capacity = frames
        buf = .allocate(capacity: frames * 2)
        buf.initialize(repeating: 0, count: frames * 2)
        head.initialize(to: 0)
        tail.initialize(to: 0)
    }

    deinit {
        buf.deallocate()
        head.deallocate()
        tail.deallocate()
    }

    var fill: Int { OSMemoryBarrier(); return head.pointee - tail.pointee }

    func write(_ src: UnsafePointer<Float>, frames n: Int) {
        var h = head.pointee
        for i in 0..<n {
            let k = (h % capacity) * 2
            buf[k] = src[i * 2]
            buf[k + 1] = src[i * 2 + 1]
            h += 1
        }
        OSMemoryBarrier()
        head.pointee = h
    }

    /// Deinterleaves up to `n` frames into `l`/`r`; the rest is silence.
    func read(l: UnsafeMutablePointer<Float>, r: UnsafeMutablePointer<Float>, frames n: Int, keep: Int) {
        OSMemoryBarrier()
        var t = tail.pointee
        let h = head.pointee
        if h - t > keep * 4 { t = h - keep }     // far behind the other clock: catch up
        for i in 0..<n {
            if t < h {
                let k = (t % capacity) * 2
                l[i] = buf[k]
                r[i] = buf[k + 1]
                t += 1
            } else {
                l[i] = 0
                r[i] = 0
            }
        }
        OSMemoryBarrier()
        tail.pointee = t
    }
}

final class OthersPlayer {
    private var engine: AVAudioEngine?
    private var proc: AudioDeviceIOProcID?
    private var device: AudioObjectID = 0
    private let ring = FrameRing(frames: 1 << 17)
    private(set) var targetName = ""

    var isRunning: Bool { engine != nil }

    static func microphoneAllowed() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return true
        case .notDetermined: return await AVCaptureDevice.requestAccess(for: .audio)
        default: return false
        }
    }

    /// Plays ScreamMusic's input (other apps) on `target`. Returns an error text, or nil.
    func start(scream: AudioObjectID, target: AudioObjectID) -> String? {
        stop()
        device = scream
        let rate = ScreamDevice.nominalRate(scream) ?? 44100
        targetName = SystemAudio.name(target)

        // read the input only: our IOProc must not take part in the output
        var usageSize: UInt32 = 0
        var usageAddr = ScreamDevice.address(kAudioDevicePropertyIOProcStreamUsage, kAudioObjectPropertyScopeOutput)

        let ring = self.ring
        var procID: AudioDeviceIOProcID?
        var st = AudioDeviceCreateIOProcIDWithBlock(&procID, scream, nil) { _, input, _, _, _ in
            let abl = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input))
            guard let b = abl.first, let data = b.mData, b.mNumberChannels == 2 else { return }
            ring.write(data.assumingMemoryBound(to: Float.self), frames: Int(b.mDataByteSize) / 8)
        }
        guard st == noErr, let procID else { return "Could not read the ScreamMusic input (\(st))." }
        proc = procID
        if AudioObjectGetPropertyDataSize(scream, &usageAddr, 0, nil, &usageSize) == noErr, usageSize > 0 {
            let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(usageSize), alignment: 8)
            defer { raw.deallocate() }
            let usage = raw.assumingMemoryBound(to: AudioHardwareIOProcStreamUsage.self)
            usage.pointee.mIOProc = unsafeBitCast(procID, to: UnsafeMutableRawPointer.self)
            if AudioObjectGetPropertyData(scream, &usageAddr, 0, nil, &usageSize, raw) == noErr {
                let n = Int(usage.pointee.mNumberStreams)
                withUnsafeMutablePointer(to: &usage.pointee.mStreamIsOn) { p in
                    let flags = UnsafeMutableRawPointer(p).assumingMemoryBound(to: UInt32.self)
                    for i in 0..<n { flags[i] = 0 }
                }
                AudioObjectSetPropertyData(scream, &usageAddr, 0, nil, usageSize, raw)
            }
        }

        let engine = AVAudioEngine()
        if let au = engine.outputNode.audioUnit {
            var dev = target
            AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, UInt32(MemoryLayout<AudioObjectID>.size))
        }
        guard let fmt = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2) else { return "Bad format." }
        let keep = Int(rate / 20)      // ~50 ms
        let source = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
            let bufs = UnsafeMutableAudioBufferListPointer(abl)
            guard bufs.count >= 2, let l = bufs[0].mData, let r = bufs[1].mData else { return noErr }
            ring.read(l: l.assumingMemoryBound(to: Float.self), r: r.assumingMemoryBound(to: Float.self), frames: Int(frameCount), keep: keep)
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: fmt)
        do {
            try engine.start()
        } catch {
            AudioDeviceDestroyIOProcID(scream, procID)
            proc = nil
            return "Could not play on \(targetName): \(error.localizedDescription)"
        }
        st = AudioDeviceStart(scream, procID)
        if st != noErr {
            engine.stop()
            AudioDeviceDestroyIOProcID(scream, procID)
            proc = nil
            return "Could not start reading ScreamMusic (\(st))."
        }
        self.engine = engine
        elog("others: playing other apps on \(targetName) (ScreamMusic at \(Int(rate)) Hz)")
        return nil
    }

    func stop() {
        if let p = proc {
            AudioDeviceStop(device, p)
            AudioDeviceDestroyIOProcID(device, p)
            proc = nil
        }
        if let e = engine {
            e.stop()
            engine = nil
            elog("others: stopped")
        }
    }
}
