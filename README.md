# ScreamMusic

[中文](README.zh-CN.md)

A macOS menu-bar app with a small audio driver. It adds a sound card called **ScreamMusic** to the system and keeps Apple Music bit-perfect at each track's own sample rate and bit depth. What is played into it is sent unchanged over the Scream protocol (UDP).

Requires macOS 26 or later. Protocol-compatible with [screamalsa](https://github.com/ayanamilee/screamalsa). License: GPL-3.0.

## Download

The zip in [Releases](https://github.com/ayanamilee/ScreamMusic/releases). Ad-hoc signed, not notarized: unzip, right-click the app, Open.

**Use 0.4.7.** 0.5.0 and 0.5.1 have a known problem: the device clock runs slow, so the receiver underruns and the sound is garbled. Those releases are kept for the record; do not install them.

## Receiver

Fill in the IP and port in the app, Apply, and pick the header the receiver expects:

| Receiver | Header |
|---|---|
| scream2diretta (s2d, without `--legacy`) | **Extended** (6 bytes, default) |
| Original scream, `scream -L`, apscream | **Original** (5 bytes) |

s2d and original scream listen on **4010** by default. This app sends to **4011** by default. Use the same port on both sides.

Original sends 32-bit PCM only. Extended sends 16 / 24 / 32-bit per track.

## Use

1. Open the app and click **Install Driver…** (administrator password; system audio stops for a moment).
2. Fill in the receiver and click Apply.
3. **Apple Music**: turn on Apple Music mode. Allow control of Music; if other apps should play on another output, also allow the Microphone.

Set the volume on the amplifier or the receiver. The sound card has no volume — it would no longer be bit-perfect.

Turning the mode off, or quitting, restores the previous default output. **Remove Driver…** removes only the sound card. **Uninstall…** restores the outputs, removes the driver, deletes settings and logs, and deletes the app.

## Apple Music mode

When it is on:

1. The system default output becomes ScreamMusic (the previous output is remembered and restored when the mode goes off). That is how Apple Music's sound is captured.
2. The device follows each track's sample rate and bit depth. When the rate changes, the track is held at the gap between tracks, the device is switched, and the track is rewound and replayed — the first fraction of a second at the old rate never reaches the receiver.
3. Other apps play on the output you choose (the one from before the mode, built-in, or a specific device), or are muted.

## Notes

- Stereo, UDP only. Sample rates 44.1–384 kHz.
- Bit-perfect needs: Music volume 100, Sound Check, EQ, Crossfade and Dolby Atmos off.
- Apple Music streams may start with a few seconds at a lower rate (48 kHz, then up to 96 kHz). Music itself leaves a gap of about 0.1 s at that upgrade. Tracks at the same rate join seamlessly.
- A rate change starts with 0.5 s of digital silence at the new rate, so the receiver and the DAC relock before the music. Adjustable in Advanced (Off / 250 / 500 / 1000 ms). Requires the current driver.
- Played tracks remember their confirmed format (Rate Library, in `~/Library/Application Support/ScreamMusic/`): picking a track by hand no longer waits for its decoder log. It is only a hint — a gapless transition trusts the track's own pre-roll, and a wrong entry is corrected by the grid check and dropped. Clear it in Advanced.
- After dragging the progress bar there can be a click: Music does not fade in at the new position. Left alone to stay bit-perfect.
- Updating the driver restarts coreaudiod and silences the system for a minute or two. If it does not come back, run `sudo killall AirPlayXPCHelper`.
- If the menu-bar icon is hidden, opening the app again brings up its window.

Build from source: `./build.sh`, output `build/ScreamMusic.app`. Design notes: `DESIGN.md`.
