# Architecture

```
┌──────────────────────────────┐  named pipe (per-user DACL)   ┌───────────────────────────────────────────┐
│ dB-Deck.exe  (Tauri 2)        │  NDJSON protocol v1           │ dbdeck-engine.exe  (C++20, separate process)│
│  React UI  ⇄  Rust bridge     │ ────────────────────────────▶ │  control thread: EngineService            │
│  - renders engine state only  │ ◀──── state / meters /        │   ├ ConfigStore (atomic JSON, LKG)        │
│  - tray, close-to-tray        │       windowsVolume events    │   ├ DeviceManager (IMMNotificationClient) │
│  - engine supervisor/respawn  │                               │   ├ SessionManager (IAudioSessionManager2)│
└──────────────────────────────┘                               │   ├ EndpointVolumeController x2           │
                                                                │   ├ CaptureStreams  (MMCSS threads)       │
                                                                │   ├ AudioEngine: RenderStreams + clock    │
                                                                │   └ Mixer + RuntimeRegistry (RT graph)    │
                                                                └───────────────────────────────────────────┘
```

## Processes

- **Engine** (`dbdeck-engine.exe`): owns all audio and Windows integration and is the single source of truth. It keeps running when the window closes and if the UI crashes. One instance per user, enforced by a named mutex plus `FILE_FLAG_FIRST_PIPE_INSTANCE` on the pipe.
- **Desktop app** (`dB-Deck.exe`): Tauri shell. Rust connects to the engine pipe, starts the engine if it isn't running (rate-limited respawn), forwards events to the webview, and owns the tray. The React UI only renders confirmed engine state. Every change is a command, and the UI updates from the response or the next `state` event.

## Three kinds of channel control (deliberately distinct)

1. **Session control**: an App channel without capture. dB-Deck sets the app's Windows session volume and mute (`ISimpleAudioVolume`) and reads its peak (`IAudioMeterInformation`). dB-Deck **does not receive the samples**. The app keeps playing on its own Windows endpoint. Its sends are shown as *logical* and labelled "Windows".
2. **Capture / managed audio**: microphones (WASAPI capture), playback-endpoint loopback (e.g. a virtual cable's playback side) and per-app process loopback. PCM flows through the engine graph (effects, buses, sends, meters).
3. **Fully managed routing via dB-Deck virtual endpoints**: requires the driver (not built). Today, third-party virtual cables plus loopback provide the same topology.

Double-audio guard: a captured app's copy is never sent to a bus that plays on the endpoint where the app's original audio already plays. The send is reported as `blocked` with a reason.

## Real-time graph

- `ProfileConfig` (declarative model) is compiled by `compileGraph` into a `CompiledGraph`: flat arrays of raw pointers plus preallocated scratch buffers.
- The graph is published with an atomic pointer swap. The clock thread adopts it at the start of the next block and acknowledges the epoch. The control thread frees older graphs only after that acknowledgement, so the audio thread never allocates, frees, locks or waits.
- Continuous parameters (fader, trim, balance, sends, effect parameters) are atomics in long-lived `RuntimeRegistry` objects shared across graph versions. Moving a fader never rebuilds the graph. Smoothing state survives swaps.
- Processing order: channels (pull → effects → pre-fader copy → fader/balance → meter → sends), then buses in topological order (bus inputs → effects → fader → meter), then outputs (sum per device × master ramp, clamp).

## Clocking

- When Main has a connected device, that device's event-driven render thread calls `Mixer::process` directly (no extra buffering).
- Otherwise a high-resolution waitable-timer thread drives the mixer so other buses, inputs and meters keep working.
- Every other device (capture or extra outputs) is bridged by an `AdaptiveStream`: an SPSC ring plus a Hermite resampler whose ratio is steered by a PI controller toward a target fill. This converts sample rates and absorbs clock drift.

## Device policy

- Endpoints are stored by stable IMMDevice ID (friendly name kept for display only).
- If a bus's device disappears, the bus becomes `disconnected`, its audio stops, routing is kept and it reconnects when the device returns. Fallbacks are used only if the user listed them explicitly. The Windows default is never used implicitly.
- Render streams fade in on (re)connect to avoid pops.

## Windows master volume

`EndpointVolumeController` binds `IAudioEndpointVolume` plus a change callback to (a) the Windows default playback endpoint, rebound on `OnDefaultDeviceChanged`, and (b) optionally the Main output device. dB-Deck's own changes carry a private event-context GUID. Notifications are applied to state and pushed to the UI immediately as `windowsVolume` events. The engine never issues a volume command in response to a notification, so loops can't occur. Redundant sets are skipped. Endpoints without a working software volume are reported `available: false` with a reason. This is separate from the dB-Deck Main bus gain.

## Persistence

`%APPDATA%\dB-Deck\config.json` (schema version 1, see `shared/schemas/config.schema.json`) is written atomically (temp file, flush, `MoveFileEx`). `config.lkg.json` is refreshed after every successful load. A malformed file is copied aside (`config.corrupt-<time>.json`) and the last known good copy is used. Logs: `%APPDATA%\dB-Deck\logs\engine.log` (previous run kept as `.prev.log`).

See also [AUDIO_ENGINE.md](AUDIO_ENGINE.md), [DECISIONS.md](DECISIONS.md), [DRIVER_PLAN.md](DRIVER_PLAN.md).
