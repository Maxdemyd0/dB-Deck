# Decisions

| # | Decision | Why |
|---|---|---|
| 1 | Engine is a separate process; the UI is a thin client over a named pipe | Audio must survive window close and UI crashes. The UI can be restarted safely. |
| 2 | Newline-delimited JSON over a per-user named pipe (protected DACL: user and SYSTEM, `PIPE_REJECT_REMOTE_CLIENTS`, first-instance flag, `hello` handshake, 1 MB limit) | Simple, debuggable (`scripts/engine-cli.py`), and secure enough for local IPC without admin rights. |
| 3 | Engine-authoritative state: the UI renders only `state` events and responses | Requirement: never show unconfirmed state. Full state is pushed after each command (throttled to 20 Hz). Meters are separate, droppable, 30 Hz. |
| 4 | Lock-free compiled graph with epoch-acknowledged swap, plus atomic parameters in long-lived runtime objects | No allocation, locks or waits on the audio thread. Faders never rebuild the graph. |
| 5 | Main output device clocks the mixer; timer clock as fallback; `AdaptiveStream` (Hermite plus PI drift control) for every other clock domain | Lowest latency on Main. One mechanism handles sample-rate conversion and drift. |
| 6 | App channel fader = Windows session volume (raw scalar); captured app PCM enters at unity | Matches user expectations from the Volume Mixer and avoids double attenuation. |
| 7 | Double-audio guard blocks captured-app sends to the endpoint the app already plays on | Process loopback does not silence the original output. |
| 8 | Feedback guard rejects loopback of a device routed (directly or via bus chains) back to the same device; bus cycles are rejected | Safety: no feedback loops. |
| 9 | No implicit fallback to the Windows default. Only user-listed fallbacks | Requirement: never play on unauthorized hardware. |
| 10 | Windows master volume via `IAudioEndpointVolume` with a private event-context GUID; no commands in response to notifications | Bidirectional sync without echo loops. |
| 11 | No virtual driver code shipped | WDK unavailable and no safe test VM. A fake device is forbidden. Third-party cables are supported instead (see DRIVER_PLAN). |
| 12 | Static CRT for the engine; nlohmann/json pinned by SHA-256 via FetchContent; minimal in-house test harness | Reproducible build, no VC++ redistributable, minimal dependencies. |
| 13 | Per-user NSIS installer (no admin); hooks stop the engine before install/uninstall; user settings are kept | Normal runtime needs no elevation. Uninstall doesn't leave a running engine behind. |
| 14 | Fader taper for managed audio = position² | Simple, monotonic, about −12 dB at mid travel. Shown in dB in the UI. |
| 15 | Groups are gain/mute stages applied to member channels (not separate PCM buses) | Mathematically equal to a post-fader submix without group effects. Honest in session-only mode, where they scale session volumes. |
