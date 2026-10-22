# Testing

## Automated

| Suite | Command | Result (2026-10-16) |
|---|---|---|
| Engine core: unit + audio (generated PCM through the real mixer) | `build\engine-Release\dbdeck-tests.exe` | **48/48 pass** |
| Windows integration (real devices) | `build\engine-Release\dbdeck-wintests.exe` | **7/7 pass** |
| UI unit (formatting/taper parity with engine) | `cd apps/desktop && npx vitest run` | **3/3 pass** |
| UI type check (strict) | `npx tsc --noEmit` | pass |

Core coverage: ring buffer; int16/int24/float conversion; mono and 5.1/7.1 mapping; gain/taper; click-free smoothing; resampling 44.1→48 kHz (pitch and level); 0.1 % clock drift over 60 s without underruns; starvation recovery; each effect's behaviour (EQ +12 dB at the center frequency, compressor ratio, limiter ceiling, gate, bypass); app matching; bus ordering; cycle and feedback rejection; protocol validation; two-source mixing; per-bus independence; mute ramp; pre-fader sends; groups; bus-to-bus; clipping clamp and count; master mute; double-audio guard; graph swap during processing and channel removal while playing; 64 channels × 33 buses; scenes; config round trip, validation, clamping, repair, atomic save, corruption recovery.

## Windows integration (run on the dev machine)
Machine: Windows 11 Home 10.0.26200, 15 playback / 14 recording endpoints (Realtek, NVIDIA HDMI, USB mics, Voicemeeter VAIO, Voicemod, NVIDIA Broadcast).

- Device enumeration: real endpoints, default device detected.
- **Session volume (Milestone 1):** a child process opens a real session; dB-Deck sets 0.37 and mute; an independent enumeration reads back 0.37 and muted. Also verified in the desktop app: dragging Opera GX's fader set its Windows session volume to 0.444.
- **Render:** a 1 kHz tone at 0.1 sent through the mixer to "Dummy Output (Voicemod)", captured by loopback: amplitude 0.0999957, 1000 Hz, **render latency 22.8–23.7 ms** across runs (QPC-timestamped).
- **Capture:** the engine's own loopback `CaptureStream` delivered the 750 Hz tone into the engine at amplitude 0.099 (48 kHz float).
- **Windows master volume:** dB-Deck → Windows (read back by an independent `IAudioEndpointVolume`); external change → dB-Deck notification tagged external; `VK_VOLUME_DOWN` key → notification; redundant set produces no notification; mute both directions; original volume restored. In the running app, three Volume Up key presses moved the engine's value 0.50 → 0.56.
- **IPC:** commands before `hello` rejected; malformed JSON → `bad_request`; wrong protocol version rejected; out-of-range args → `bad_args`; unknown ID → `not_found`; a new channel defaults to Main; loopback of the Main device → `rejected` (feedback); undo; shutdown persists config.
- **Process lifecycle (release build):** the app started the bundled engine; killing the UI left the engine running (same PID); a relaunched UI reconnected; after an engine shutdown the running UI restarted it.

Environment variables: `DBDECK_TEST_RENDER_DEVICE` (substring of the device name, default "Dummy Output"), `DBDECK_TEST_SKIP_MASTER=1`.

## Voicemeeter import
- Unit tests (synthetic Potato file): strips, buses, routing, gains, mutes, missing devices, skipped/unsupported features, invalid files.
- Real file (`Documents\Voicemeeter\VoicemeeterPotato_LastSettings.xml` on the dev machine), imported into a temporary engine: 5 channels and 6 buses that match the Voicemeeter routing (VAIO3 → USBMIC1 + Realtek + WH202A; AUX +11.6 dB → Main + B1; Voicemod mic → B1/B2; NVIDIA Broadcast mic → B3). A3 (WH202A, not connected) is kept by name and binds when the device appears.
- **Live switch-over on the dev machine (2026-10-17):** installed dB-Deck (per-user NSIS, silent), imported the real file, closed Voicemeeter, loaded the profile. All outputs and inputs connected. Audio played into "Voicemeeter VAIO3 Input" **with Voicemeeter closed** was captured by dB-Deck (peak 0.48) and reached Main, Realtek and the Headphones bus. So the VAIO playback endpoints keep working as dB-Deck inputs without the Voicemeeter app.
- **VB-CABLE round trip:** 1 kHz at 0.1 into "CABLE Input" through the engine arrived on "CABLE Output" at 0.0988 / 1000.06 Hz (1 underrun at stream start). VB-CABLE exposes two playback sides ("CABLE Input", "CABLE In 16ch"), and both are recognised as feeding CABLE Output for feedback protection.

## Per-app output device
- Uses the undocumented `Windows.Media.Internal.AudioPolicyConfig` factory (the same one Windows Settings uses). The test sets the preference for its own executable, starts a child of the same executable, and checks that the child's real audio session opens on the chosen device. On the dev machine (Windows default = CABLE Input) the child played on "Dummy Output (Voicemod)" as chosen; the preference is reset afterwards.

## Virtual driver (static only)
- Built with WDK 10.0.28000 / VS 18, warnings as errors: `dbdeckvad.sys`, `.inf`, `.cat` (test-signed).
- InfVerif `/w`: INF is valid.
- Code Analysis (driver rules): no warnings in dB-Deck code. Remaining warnings are in unmodified sample code.
- Engine on this machine: `driver.installed = false`, no false cable pairs among 20 third-party virtual endpoints.
- Unit test: the recording side of a cable routed into a bus that plays into its playback side is rejected as feedback.

## QoL pass (2026-10-21)
- **Automated:** engine core 64/64 (new: solo on Main only, solo pruning, undo coalescing/redo/depth cap, structural view, strip colors, hotkey settings round trip and validation); UI vitest 10/10 (dB step math and typed-level parsing, accelerator capture, hotkey actions, device toasts).
- **IPC against an isolated engine** (`--config-dir` in a temp folder, so the user's config is untouched):
  - `reorderBuses` keeps Main first; bad colors and empty names → `bad_args`.
  - `setSolo` shows in `engine.solo` and clears.
  - 20 volume updates 30 ms apart = 1 undo step. Undo restores the level, and redo re-applies it. Undo brings back a deleted channel and a deleted scene. An extra redo → `rejected`.
  - Hotkey bindings saved; an unknown action is dropped.
- **Voicemeeter detection on the dev machine:** with Voicemeeter Potato running, pointing Main at "Speakers (USBMIC1.)" gave `AUDCLNT_E_DEVICE_IN_USE`. The engine reported `{"voicemeeterRunning": true, "devicesInUse": ["Speakers (USBMIC1.)"]}`. "Close Voicemeeter" was **not** clicked during testing (it would have closed the user's running mixer).
- Not yet checked by hand in the running app: global hotkeys while a game has focus; the Close Voicemeeter button; right-click menus; drag-to-reorder buses; clip-light latching.

## Not yet verified
- Dragging the **Windows 11 Quick Settings** slider itself. The automation tool cannot operate the shell flyout. The same mechanism (an external `IAudioEndpointVolume` change and volume keys) is verified above. Manual check: open dB-Deck, drag Quick Settings, and the toolbar's Windows volume should follow and briefly highlight.
- Process-loopback app capture end-to-end with a real app, including whether capture is before or after session volume.
- Hardware unplug and replug of the Main device, and sleep/resume. The logic is implemented (invalidation → disconnected → retry), but no device was physically unplugged during testing.
- End-to-end mic → Main latency in `low` mode.
- Long-duration stability (hours).
- Installer install and uninstall on a clean machine (the installer was built, not run).
- Virtual driver at runtime: install, audio through the cable, stop/start, removal, Driver Verifier. Needs a test-signing VM (see driver/windows/README.md).
