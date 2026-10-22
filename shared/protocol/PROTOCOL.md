# dB-Deck engine protocol v1

Transport: named pipe `\\.\pipe\dB-Deck.Engine.v1.<user SID>`, restricted to the current user and SYSTEM, remote clients rejected. UTF-8 JSON, one message per line, max 1 MiB.

```jsonc
// request
{"id": 7, "cmd": "updateChannel", "args": {"id": "…", "volume": 0.5}}
// response
{"id": 7, "ok": true, "result": {…}}
{"id": 7, "ok": false, "error": {"code": "bad_args", "message": "…"}}
// event (no id)
{"event": "state" | "meters" | "windowsVolume", "data": {…}}
```

The first command must be `hello {"protocol": 1}`. Error codes: `bad_request`, `bad_args`, `not_found`, `rejected` (valid request refused, e.g. it would create a cycle or feedback), `unsupported`, `internal`.

After every command the engine pushes a full `state` (throttled to 20 Hz otherwise). `meters` arrive at the configured rate and may be dropped for slow clients. `windowsVolume` is pushed immediately on any endpoint volume change.

## Commands
| Command | Args |
|---|---|
| hello | protocol |
| getState, getDiagnostics, getDriverState, getIcons, listDevices, listSessions, getEffectTypes | — |
| createChannel | type (`app`\|`hardwareInput`\|`endpointLoopback`\|`testTone`), name?, exeName (app), deviceId (inputs), toneHz?, groupId? |
| deleteChannel | id |
| updateChannel | id, name?, volume?, mute?, gainDb?, balance?, groupId?, appCapture?, deviceId?, exeNames?, pathContains?, toneHz?, reconnect?, color? (`#rrggbb` or "") |
| reorderChannels | ids[] |
| setSolo | id, solo? (default true) — or clear: true. Runtime only: soloed channels are the only ones heard **on Main**; other buses are untouched |
| setSend | channelId, busId, enabled?, gain? (0–2), preFader? |
| createGroup / updateGroup / deleteGroup | name, channelIds? / id, name?, volume?, mute?, collapsed? / id |
| createBus / updateBus / deleteBus | name, deviceId? / id, name?, volume?, mute?, deviceId?, fallbackDeviceIds?, color? / id (not Main) |
| reorderBuses | ids[] (Main always stays first) |
| setBusSend | fromBusId, toBusId, enabled?, gain?, preFader? |
| setMainOutput | deviceId ("" = none) |
| addEffect / updateEffect / removeEffect / moveEffect | target (`channel`\|`bus`), id, type / effectId, enabled?, params? / effectId / effectId, index |
| createProfile / renameProfile / deleteProfile / loadProfile | name, copyCurrent? / id, name / id / id |
| createScene / updateScene / deleteScene / applyScene | name / id, name?, recapture? / id / id |
| setMasterMute | mute |
| setWindowsVolume | target (`default`\|`main`), volume?, mute?, step? (`up`\|`down`) |
| updateSettings | bufferMode?, meterRateHz?, showSystemSounds?, hotkeys? (`[{accelerator, action, targetId}]`, replaces the list; invalid entries dropped) |
| setLayout | layout (opaque object) |
| closeVoicemeeter | — (asks running Voicemeeter instances to close, as if the user clicked X; never terminates them) |
| undo, redo, shutdown | — |

### Undo
Edits to channels, buses, groups, sends, effects and scenes are undoable (50 steps). Continuous edits to the same control within 1.5 s (a fader drag) merge into one step. Undoing a level, mute or send change applies live without rebuilding audio. Any new edit clears redo. Loading a profile clears both. `state.engine.undoDepth` / `redoDepth` give the counts.

### State additions (all optional for older clients)
- `engine.solo`: soloed channel IDs.
- `engine.conflicts`: `{voicemeeterRunning, devicesInUse: [device names]}`. A device is "in use" when Windows refused it with `AUDCLNT_E_DEVICE_IN_USE` (another app holds it exclusively). Retried every 2 s, and immediately when Voicemeeter exits.
- `engine.redoDepth`, `settings.hotkeys`, and `color` on channels and buses.

TypeScript types for the state: `apps/desktop/src/engine/types.ts`. A reference client: `scripts/engine-cli.py`.
