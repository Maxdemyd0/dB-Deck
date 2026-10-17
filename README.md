# dB-Deck

**Unlimited channels. Real audio routing. Free software. Simple interface.**

dB-Deck is a free, open-source (MIT) audio mixer for Windows 11. It aims to combine the simplicity of the Windows Volume Mixer with Voicemeeter-style routing, without artificial strip or bus limits. It has no account, no subscription and no telemetry.

> **Status: early (0.1.0).** The app-volume mixer, managed audio graph, Windows master-volume sync and desktop app work and are tested on real hardware. dB-Deck's **virtual cable driver builds but has not been installed or tested yet** (it needs a test-signing VM). Meanwhile, existing virtual cables (VB-Audio, Voicemeeter) can be used as inputs and outputs. See [docs/HANDOFF.md](docs/HANDOFF.md) for exactly what works.

## What works today

| Feature | State |
|---|---|
| Per-app volume and mute (Windows audio sessions), persistent channels that reconnect when apps restart | ✅ verified on real sessions |
| Available Apps panel with icons, search, one-click add | ✅ |
| Windows master volume ⇄ dB-Deck (Quick Settings, volume keys, mute), default-device rebinding | ✅ verified |
| Managed audio engine: microphones, playback loopback, per-app capture, test tones | ✅ verified (capture + render through real endpoints) |
| Unlimited buses, per-channel sends (pre/post), bus-to-bus, groups, master mute | ✅ unit + audio tested |
| Main output independent of the Windows default, explicit fallbacks only, auto-reconnect | ✅ |
| DSP: gain, 3-band EQ, gate, compressor, limiter (click-free bypass) | ✅ audio tested |
| Profiles, scenes, undo, atomic config with last-known-good recovery | ✅ |
| Tray: close-to-tray, mute all, profile switch, Main status, exit with warning | ✅ |
| Engine runs as a separate process; survives UI crashes; UI restarts the engine if it dies | ✅ verified |
| NSIS installer (per-user) | ✅ builds |
| dB-Deck virtual cable driver | 🟡 builds (test-signed, InfVerif valid); **not installed or runtime-tested**. See [driver/windows](driver/windows/README.md) |
| On-demand creation of many virtual devices | ❌ not started (see [DRIVER_PLAN](docs/DRIVER_PLAN.md)) |

## Layout

```
apps/desktop/        Tauri 2 shell (Rust) + React/TypeScript UI
engine/              C++20 audio engine
  core/              platform-independent: graph, mixer, DSP, config, protocol
  win/               WASAPI, sessions, devices, endpoint volume, named-pipe IPC
  tests/             unit, audio and Windows integration tests
shared/protocol/     IPC protocol reference
shared/schemas/      configuration JSON schema
installer/           NSIS hooks
driver/windows/      virtual cable driver (WDK, MS-PL, derived from SimpleAudioSample)
docs/                architecture, decisions, testing, handoff
scripts/             build scripts and a dev CLI for the engine
```

## Building

Requirements: Windows 11 x64, Visual Studio 2022+ with the C++ workload (MSVC, CMake, Ninja), Windows SDK 10.0.22621+, Rust (stable, MSVC), Node.js 20+. CMake downloads nlohmann/json 3.11.3 (pinned by hash).

```bash
scripts\build-release.cmd
```

This builds the engine, runs the core tests, stages the engine as a Tauri sidecar and produces `apps/desktop/src-tauri/target/release/bundle/nsis/dB-Deck_0.1.0_x64-setup.exe`.

Development:

```bash
scripts\build-engine.cmd Release
```

```bash
cd apps/desktop && npm install && npx tauri dev
```

In dev mode the UI starts `build/engine-Release/dbdeck-engine.exe` automatically.

Driver (needs the WDK; builds and test-signs only, installs nothing):

```bash
scripts\build-driver.cmd
```

## Tests

```bash
build\engine-Release\dbdeck-tests.exe
```

```bash
build\engine-Release\dbdeck-wintests.exe
```

```bash
cd apps/desktop && npx vitest run
```

The Windows integration tests use real devices: they play a quiet tone to a device named "Dummy Output" (override with `DBDECK_TEST_RENDER_DEVICE`) and briefly change and restore the Windows volume (skip with `DBDECK_TEST_SKIP_MASTER=1`). See [docs/TESTING.md](docs/TESTING.md).

## License

MIT. See [LICENSE](LICENSE). Exception: `driver/windows/dbdeck-vad` is derived from Microsoft sample code and is licensed under MS-PL.
