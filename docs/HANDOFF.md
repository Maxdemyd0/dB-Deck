# Handoff: dB-Deck 0.1.0

## Milestone status
| Milestone | Status |
|---|---|
| 1. Genuine Windows mixer (sessions, Available Apps, persistent channels, volume and mute, device selection, tray, persistence) | **Done, verified on real sessions.** |
| + Windows master volume sync (added requirement) | **Done, verified** (dB-Deck⇄Windows, volume keys, mute, loop-free). Dragging the Quick Settings slider itself is not automated; see TESTING. |
| 2. Real managed audio graph (capture, playback, multiple channels and buses, routing, meters, gain, validation) | **Done.** Two-source mixing and bus independence verified with real PCM in tests. Render and capture verified through real Windows endpoints. |
| 3. Virtual audio infrastructure (driver) | **Not started in code. Blocked:** no WDK, no isolated test VM, no signing. Plan: DRIVER_PLAN.md. Interim: third-party virtual cables are supported. |
| 4. Polished app (profiles, scenes, effects, groups, routing UI, hotkeys, diagnostics, installer, docs) | **Mostly done.** The installer is built but not install-tested on a clean machine. |

## Is dB-Deck a Voicemeeter replacement yet?
**No.** It replaces the Volume Mixer use case and offers real managed routing for microphones, loopback-captured devices and captured apps. It cannot yet create its own virtual devices, which is Voicemeeter's defining feature. Until the driver exists, routing an arbitrary app independently requires a third-party virtual cable.

## Build and run
See README. `scripts\build-release.cmd` produces the installer. In dev, `npx tauri dev` uses `build/engine-Release/dbdeck-engine.exe`.
Engine CLI for debugging: `python scripts/engine-cli.py getState`, or `--events 10`.

## Known limitations and risks
- **Process-loopback capture:** implemented (process-tree roots, re-attached as processes come and go) but not validated end-to-end with a real app. Whether it captures before or after Windows session volume is assumed, not measured.
- **Device unplug/replug and sleep/resume:** handled by invalidation detection plus 2 s retries and notifications, but not physically tested.
- **Resampler:** Hermite with no anti-aliasing filter. Fine for 44.1/48 kHz; it can alias when downsampling from 96 kHz or higher.
- **Limiter:** no lookahead.
- **Full state push per command:** fine for typical sizes. A large profile with a fast fader drag sends about 20 state messages/s, and a delta protocol would scale better.
- **Engine respawn:** if the user ends the engine while the UI runs, the UI restarts it. That is intended crash recovery. Tray "Exit" stops both.
- **App matching:** by executable name (plus optional path filter). Two different apps with the same exe name share a channel unless a path filter is set.
- **Third-party cable pairing** (cable playback side ↔ recording side) is not detected, so feedback through an external cable (bus → cable in, cable out → input → same bus) is not prevented. Only same-device loopback loops are blocked.
- `tauri.conf.json` warns that `STATIC_VCRUNTIME` is deprecated. This is cosmetic.

## Next engineering steps
1. Manually verify Quick Settings ⇄ dB-Deck, device unplug/replug, sleep/resume, and app capture with Discord/a game. Add results to TESTING.md.
2. Install-test the NSIS installer on a clean VM (Start with Windows, uninstall stops the engine and removes the Run key).
3. Driver stage 1 in a VM (DRIVER_PLAN steps 1–2).
4. Delta state events; polyphase resampler; limiter lookahead (optional, adds latency).
5. Latency measurement for mic → Main in `low` mode; publish figures.
6. Detect virtual-cable pairs (shared container ID or name heuristics) to extend feedback protection.
