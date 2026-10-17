// Types mirroring the engine's JSON state (protocol v1).
// See shared/protocol/PROTOCOL.md. The engine is authoritative: the UI only
// renders what these structures say.

export type ChannelType = "app" | "hardwareInput" | "endpointLoopback" | "testTone";

export interface Send {
  busId: string;
  enabled: boolean;
  gain: number;
  preFader: boolean;
}

export interface Effect {
  id: string;
  type: "gain" | "eq" | "gate" | "compressor" | "limiter";
  enabled: boolean;
  params: Record<string, number>;
}

export interface Channel {
  id: string;
  name: string;
  type: ChannelType;
  match: { exeNames: string[]; pathContains: string };
  appCapture: boolean;
  deviceId: string;
  deviceName: string;
  toneHz: number;
  volume: number;
  gainDb: number;
  balance: number;
  mute: boolean;
  groupId: string;
  sends: Send[];
  effects: Effect[];
  reconnect: "auto" | "manual";
}

export interface Group {
  id: string;
  name: string;
  volume: number;
  mute: boolean;
  collapsed: boolean;
}

export interface Bus {
  id: string;
  name: string;
  isMain: boolean;
  volume: number;
  mute: boolean;
  deviceId: string;
  deviceName: string;
  fallbackDeviceIds: string[];
  busSends: Send[];
  effects: Effect[];
}

export interface Scene {
  id: string;
  name: string;
}

export interface Profile {
  id: string;
  name: string;
  channels: Channel[];
  groups: Group[];
  buses: Bus[];
  scenes: Scene[];
  layout: Record<string, unknown>;
}

export type SendState = "active" | "disabled" | "session-only" | "blocked" | "no-output";

export interface ChannelRuntime {
  status: string; // active | idle | offline | connected | disconnected | error | generated | captured
  capability: "managed" | "session" | "none";
  error: string;
  sessions: { pid: number; deviceId: string; deviceName: string; state: string; volume: number; mute: boolean }[];
  iconKey: string;
  sends: Record<string, { state: SendState; reason: string }>;
  captures: { pid: number; format: string; bufferMs: number; status: string; stream: StreamStats }[];
}

export interface BusRuntime {
  status: "connected" | "fallback" | "disconnected" | "no-destination" | "error";
  activeDeviceId: string;
  activeDeviceName: string;
  usingFallback: boolean;
  error: string;
  outputLatencyMs: number;
}

export interface StreamStats {
  underruns: number;
  overruns: number;
  resyncs: number;
  fillFrames: number;
  targetFrames: number;
  ratio: number;
  starving: boolean;
}

export interface Device {
  id: string;
  name: string;
  adapterName: string;
  flow: "render" | "capture";
  state: "active" | "unplugged" | "disabled" | "notpresent";
  isDefault: boolean;
  isDefaultComms: boolean;
  likelyVirtual: boolean;
  dbdeckDriver: boolean;
  format: string;
}

export interface AvailableApp {
  exeName: string;
  displayName: string;
  iconKey: string;
  state: "active" | "inactive";
  pids: number[];
  devices: string[];
  system: boolean;
}

export interface EndpointVolume {
  bound: boolean;
  deviceId: string;
  deviceName: string;
  available: boolean;
  unavailableReason: string;
  volume: number;
  percent: number;
  mute: boolean;
  levelDb: number;
  minDb: number;
  maxDb: number;
  steps: number;
  hardwareVolume: boolean;
  lastChangeSource: "dbdeck" | "external" | "";
  changeCount: number;
}

export interface WindowsVolume {
  default: EndpointVolume;
  main: EndpointVolume;
}

export interface EngineState {
  engine: {
    version: string;
    protocol: number;
    pid: number;
    clock: "main-output" | "timer";
    sampleRate: number;
    healthy: boolean;
    masterMute: boolean;
    warnings: string[];
    undoDepth: number;
    iconsVersion: number;
  };
  settings: { bufferMode: "low" | "normal" | "safe"; meterRateHz: number; showSystemSounds: boolean };
  profiles: { id: string; name: string }[];
  activeProfileId: string;
  profile: Profile;
  runtime: { channels: Record<string, ChannelRuntime>; buses: Record<string, BusRuntime> };
  devices: Device[];
  availableApps: AvailableApp[];
  windowsVolume: WindowsVolume;
  driver: {
    installed: boolean;
    status: "installed" | "not-installed";
    message: string;
    endpoints: Device[];
    cablePairs: { recording: string; playback: string }[];
    thirdPartyVirtualEndpoints: Device[];
  };
}

export interface MeterValue {
  peakL: number;
  peakR: number;
  rmsL: number;
  rmsR: number;
  clip: boolean;
  source?: "engine" | "session";
}

export interface Meters {
  channels: Record<string, MeterValue>;
  buses: Record<string, MeterValue>;
}

export interface EngineError {
  code: string;
  message: string;
}
