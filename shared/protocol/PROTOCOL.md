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
| updateChannel | id, name?, volume?, mute?, gainDb?, balance?, groupId?, appCapture?, deviceId?, exeNames?, pathContains?, toneHz?, reconnect? |
| reorderChannels | ids[] |
| setSend | channelId, busId, enabled?, gain? (0–2), preFader? |
| createGroup / updateGroup / deleteGroup | name, channelIds? / id, name?, volume?, mute?, collapsed? / id |
| createBus / updateBus / deleteBus | name, deviceId? / id, name?, volume?, mute?, deviceId?, fallbackDeviceIds? / id (not Main) |
| setBusSend | fromBusId, toBusId, enabled?, gain?, preFader? |
| setMainOutput | deviceId ("" = none) |
| addEffect / updateEffect / removeEffect / moveEffect | target (`channel`\|`bus`), id, type / effectId, enabled?, params? / effectId / effectId, index |
| createProfile / renameProfile / deleteProfile / loadProfile | name, copyCurrent? / id, name / id / id |
| createScene / updateScene / deleteScene / applyScene | name / id, name?, recapture? / id / id |
| setMasterMute | mute |
| setWindowsVolume | target (`default`\|`main`), volume?, mute?, step? (`up`\|`down`) |
| updateSettings | bufferMode?, meterRateHz?, showSystemSounds? |
| setLayout | layout (opaque object) |
| undo, shutdown | — |

TypeScript types for the state: `apps/desktop/src/engine/types.ts`. A reference client: `scripts/engine-cli.py`.
