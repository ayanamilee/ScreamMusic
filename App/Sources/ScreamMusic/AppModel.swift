//
//  AppModel.swift
//  ScreamMusic
//
//  Polls the driver's status and holds what the menu-bar panel edits.
//

import CoreAudio
import Foundation

@MainActor
final class AppModel: ObservableObject {

    @Published private(set) var device: AudioObjectID?
    @Published private(set) var status = DriverStatus()
    @Published private(set) var rate: Double?
    @Published private(set) var hogOwner: pid_t?
    /// The driver's current destination; `edit` is what the panel shows until Apply.
    @Published private(set) var destination = Destination()
    @Published var edit = Destination()
    @Published private(set) var applyError: String?

    let installer = DriverInstaller()
    let musicMode = MusicMode()
    private var timer: Timer?

    init() {
        poll()
        edit = destination
        musicMode.launch()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func poll() {
        musicMode.checkDriver()
        let found = ScreamDevice.find()
        if found != device {
            device = found
            installer.refresh()
        }
        guard let d = found else {
            status = DriverStatus()
            rate = nil
            hogOwner = nil
            return
        }
        status = DriverStatus(ScreamDevice.get(d, ScreamDevice.status)) ?? DriverStatus()
        rate = ScreamDevice.nominalRate(d)
        hogOwner = ScreamDevice.hogOwner(d)
        if let dest = Destination(ScreamDevice.get(d, ScreamDevice.destination)), dest != destination {
            let editing = edit != destination
            destination = dest
            if !editing { edit = dest }
        }
    }

    func applyDestination() {
        guard let d = device else { return }
        let st = ScreamDevice.set(d, ScreamDevice.destination, edit.plist)
        applyError = st == noErr ? nil : "The driver refused this destination (\(st)). Check the IP address and port."
        poll()
        if st == noErr { edit = destination }
    }

    func setRate(_ r: Double) {
        guard let d = device else { return }
        ScreamDevice.setNominalRate(d, r)
        poll()
    }
}
