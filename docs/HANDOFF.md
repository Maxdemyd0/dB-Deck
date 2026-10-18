# Handoff: dB-Deck 0.1.0

## Milestone status
| Milestone | Status |
|---|---|
| 1. Genuine Windows mixer (sessions, Available Apps, persistent channels, volume and mute, device selection, tray, persistence) | **Done, verified on real sessions.** |
| + Windows master volume sync (added requirement) | **Done, verified** (dB-Deck⇄Windows, volume keys, mute, loop-free). Dragging the Quick Settings slider itself is not automated; see TESTING. |
| 2. Real managed audio graph (capture, playback, multiple channels and buses, routing, meters, gain, validation) | **Done.** Two-source mixing and bus independence verified with real PCM in tests. Render and capture verified through real Windows endpoints. |
| 3. Virtual audio infrastructure (driver) | **Driver written and builds** (loopback cable derived from SimpleAudioSample; test-signed; InfVerif valid). Engine detects it, lists its endpoints and protects against cable feedback. **Not installed or runtime-tested. Blocked on:** a test-signing VM (installing on the dev PC needs test signing + reboot and explicit approval). Production needs an EV certificate + attestation. The Milestone 3 acceptance test (an app plays into the virtual endpoint and dB-Deck delivers it elsewhere) is not yet run. |
| 4. Polished app (profiles, scenes, effects, groups, routing UI, hotkeys, diagnostics, installer, docs) | **Mostly done.** The installer is built but not install-tested on a clean machine. |

## Is dB-Deck a Voicemeeter replacement yet?
**No.** It replaces the Volume Mixer use case and offers real managed routing for microphones, loopback-captured devices and captured apps. Its own virtual cable is built but not yet proven on a running system, and it offers one cable, not on-demand devices. Until it is installed and tested, routing an arbitrary app independently requires a third-party virtual cable.

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
- **Cable feedback protection** pairs playback/recording sides only for dB-Deck's driver and adapters named as cables with exactly one endpoint each way. A loopback device named differently (or a multi-cable adapter) is not paired, so a loop through it is not prevented.
- `tauri.conf.json` warns that `STATIC_VCRUNTIME` is deprecated. This is cosmetic.

## Replacing Voicemeeter on a Secure Boot PC (no own driver)
1. Install VB-CABLE (free, Microsoft-signed) for each virtual mic needed.
2. dB-Deck › Profiles › **Import from Voicemeeter**, then set each "B (virtual mic)" bus output to a cable's playback side.
3. Close Voicemeeter (and disable its startup), then load the imported profile. Apps keep playing to the Voicemeeter VAIO endpoints, and dB-Deck captures them.
4. In Discord/OBS, choose the cable's recording side (e.g. CABLE Output) as the microphone.

## Next engineering steps
1. Manually verify Quick Settings ⇄ dB-Deck, device unplug/replug, sleep/resume, and app capture with Discord/a game. Add results to TESTING.md.
2. Install-test the NSIS installer on a clean VM (Start with Windows, uninstall stops the engine and removes the Run key).
3. Install the driver in a test-signing VM and run the test plan in driver/windows/README.md (proves Milestone 3). Then build the cable pool (DRIVER_PLAN "Scaling").
4. Delta state events; polyphase resampler; limiter lookahead (optional, adds latency).
5. Latency measurement for mic → Main in `low` mode; publish figures.
6. Pair multi-cable adapters (e.g. VB-Audio Hi-Fi / A+B cables) by endpoint name to extend feedback protection.
