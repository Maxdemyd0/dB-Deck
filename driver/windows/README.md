# dB-Deck virtual audio driver (`dbdeck-vad`)

A kernel-mode virtual **loopback cable**. It adds one audio device, "dB-Deck Virtual Cable 1", with two endpoints:

| Endpoint | Windows type | Purpose |
|---|---|---|
| dB-Deck Virtual Input | playback (render) | Apps play into it. dB-Deck captures it (loopback) as an input channel. |
| dB-Deck Virtual Output | recording (capture) | Whatever plays into *Virtual Input* comes out here. Point OBS/Discord at it, and send a dB-Deck bus into *Virtual Input*. |

The dB-Deck engine talks to both endpoints through ordinary WASAPI (paths already verified on real devices), so the driver has **no custom user/kernel interface** (no IOCTLs, no shared memory).

## Status
- **Builds** with WDK 10.0.28000 (warnings as errors): `scripts\build-driver.cmd` produces `dbdeck-vad\x64\Release\package\dbdeckvad.{sys,inf,cat}`.
- **Static checks:** InfVerif `/w` reports the INF valid. Code Analysis with the driver rules shows no warnings in dB-Deck code. Remaining warnings are in unmodified Microsoft sample code (C26495 uninitialised-member style warnings, and C6386 in the sample tone generator, which no longer runs on the capture path).
- **Not installed and not runtime-tested.** The package is **test-signed** only. It must not be installed on a normal PC.

## Origin and license
Derived from Microsoft's **SimpleAudioSample** (microsoft/Windows-driver-samples, commit 2dc3fd3, `audio/simpleaudiosample`), licensed under the **Microsoft Public License (MS-PL)**. See `dbdeck-vad/LICENSE-MS-PL.txt`. This directory is distributed under MS-PL; the rest of dB-Deck is MIT.

dB-Deck changes:
- `Source/Utilities/LoopbackCable.{h,cpp}` (new): a non-paged, spin-lock-protected 32-bit stereo ring connecting the render DMA buffer to the capture DMA buffer. It prefills 10 ms, bounds latency to 200 ms (drops the oldest data), and fills underruns with silence. Integer PCM only.
- `minwavertstream.cpp`: render data goes to the cable instead of the sample's file writer. Capture data comes from the cable instead of the tone generator. The capture RUN/PAUSE transitions start and stop the cable.
- `adapter.cpp`: cable initialisation and cleanup. The registry switch that let the sample **write render audio to disk** was removed. The sample's stream-resource self-test was removed, because Microsoft notes a real driver must not do that.
- `minwavert.cpp`: null check flagged by code analysis (C28182).
- INF/RC: dB-Deck identity (`ROOT\dBDeckVirtualAudio`, service `dbdeckvad`, endpoint names), version 0.1.0.0.

Formats: 48 kHz stereo, 16-bit PCM (playback side) and 32-bit PCM (recording side), as in the sample.

## Installing on a test machine (NOT done by dB-Deck)
Use a disposable VM or a dedicated test PC. These steps weaken boot security and need administrator rights and a reboot:

```bash
bcdedit /set testsigning on
```

Reboot. Then trust the WDK test certificate that signed the package and install it with the WDK's `devcon` (root-enumerated device):

```bash
devcon install dbdeckvad.inf ROOT\dBDeckVirtualAudio
```

Remove it with:

```bash
devcon remove ROOT\dBDeckVirtualAudio
```

```bash
pnputil /delete-driver oem<N>.inf /uninstall
```

```bash
bcdedit /set testsigning off
```

Distribution to normal users requires an EV certificate and Microsoft attestation or HLK signing (see docs/DRIVER_PLAN.md).

## Test plan once installed (in the VM)
1. Both endpoints appear. `dbdeck-engine` reports `driver.installed = true`, plus the cable pair.
2. Play a 1 kHz tone into *Virtual Input* and record *Virtual Output*: the tone arrives at the same level (16→32-bit is exact).
3. dB-Deck: add a bus to *Virtual Input*, record *Virtual Output* in OBS, and confirm the bus audio arrives.
4. Feedback guard: capturing *Virtual Output* into that same bus is rejected.
5. Start and stop capture repeatedly, unplug and reinstall the device, and check Driver Verifier for issues.
