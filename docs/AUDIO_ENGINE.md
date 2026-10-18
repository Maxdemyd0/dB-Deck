# Audio engine

## Internal format
48 kHz, 32-bit float, interleaved stereo. Mono sources are duplicated to both sides (dual mono). Multichannel sources are folded down by speaker mask: front L/R at unity, center and surrounds at −3 dB, LFE dropped. Output to multichannel devices fills front L/R only.

## Streams (engine/win/WasapiStreams.*)
| Stream | API | Format handling |
|---|---|---|
| Hardware capture | `IAudioClient` shared, event-driven | device mix format → `deviceToStereo` → `AdaptiveStream` (rate conversion and drift) |
| Endpoint loopback | `AUDCLNT_STREAMFLAGS_LOOPBACK` on a render endpoint | same as capture. Polled every 10 ms as well, because loopback is silent (no events) when nothing plays |
| Process loopback | `ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)`, include process tree | requested directly as 48 kHz float stereo (autoconvert) |
| Render | event-driven shared mode | 48 kHz devices: mix format (IAudioClient3 minimum period in "low" mode). Other rates: request 48 kHz float with `AUTOCONVERTPCM` plus SRC |

Audio threads register with MMCSS ("Pro Audio") and set FTZ/DAZ. Device invalidation (`AUDCLNT_E_DEVICE_INVALIDATED`) marks the stream `lost`. The control thread notices within 250 ms, recompiles the graph without it, and retries every 2 s or on device notifications.

## Buffering modes
| Mode | Input ring target | Render buffer |
|---|---|---|
| low | 6 ms | IAudioClient3 minimum engine period (when the device runs at 48 kHz) |
| normal | 12 ms | 10 ms |
| safe | 30 ms | 30 ms |

## Measured latency (this development machine)
Render path, engine to the Windows endpoint mix (measured with QPC-timestamped loopback, `normal` mode, "Dummy Output (Voicemod)" device): **22.8–23.7 ms** across runs (negotiated device buffer 22 ms). Input-to-output latency adds the capture buffer and the input ring target, and is reported live as an estimate on the Diagnostics page. The ≤20 ms goal is plausible in `low` mode on 48 kHz devices, but that end-to-end figure has **not** been measured yet.

## Gain and smoothing
- Managed faders: 0–1 uses gain = position² (0.5 ≈ −12 dB); 1–1.5 boosts linearly in dB up to **+20 dB** (0 dB at two thirds of the travel). Plus a per-channel trim (−60…+24 dB). Boosted signals can clip at the output clamp; add a Limiter on the bus to avoid that. App (session) faders pass the raw scalar to Windows, like the Volume Mixer.
- All gains use one-pole per-sample smoothing (~5 ms). Effect bypass crossfades over ~4 ms. Outputs fade in over ~15 ms on (re)connect.
- Outputs are clamped to ±1.0. Bus meters count clipped blocks (shown as clip indicators).

## DSP (engine/core/dsp)
Gain, 3-band EQ (RBJ low shelf / peak / high shelf), noise gate (peak envelope, hold, range), feed-forward compressor (soft knee, makeup), and a peak limiter (instant attack without lookahead, so it adds no latency; hard ceiling clamp). Parameters are atomics, and coefficients are recomputed on the audio thread when they change.

## Counters
Every `AdaptiveStream` reports underruns, overruns, hard resyncs, fill and ratio. Render streams report stalls (event timeouts). The mixer reports per-block processing time. All are on the Diagnostics page and from `getDiagnostics`.

## Known limits
- The Hermite resampler has no anti-aliasing filter. Downsampling high-rate sources (96 kHz and above) can alias. A windowed-sinc or polyphase resampler is a planned upgrade.
- The limiter has no lookahead.
- Whether process-loopback capture is taken before or after the app's Windows session volume was not measured. The engine assumes after (app channel faders act on the session and the captured stage stays at unity). See HANDOFF.
