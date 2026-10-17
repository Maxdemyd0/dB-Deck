# Virtual audio driver

## Status (2026-10-17)
| Stage | State |
|---|---|
| WDK toolchain | WDK 10.0.28000 installed. Stock SimpleAudioSample builds. |
| dB-Deck driver source (`driver/windows/dbdeck-vad`) | **Written and builds** (test-signed package; InfVerif valid; no code-analysis warnings in dB-Deck code). |
| Install and runtime test | **Not done.** It needs a test VM or PC with test signing. Nothing was installed on the development workstation. |
| Engine integration | Done for the user-mode side: driver detection, endpoint listing, cable-pair feedback protection, UI. Endpoints are used through the already-verified WASAPI paths. |
| Multiple / on-demand cables | Not started (see "Scaling" below). |
| Production signing | Not started (EV certificate + attestation required). |

## Architecture as built: loopback cable
A PortCls WaveRT miniport derived from Microsoft's **SimpleAudioSample** (MS-PL): one render endpoint ("dB-Deck Virtual Input") and one capture endpoint ("dB-Deck Virtual Output"), both serviced by 1 ms QPC timers (no hardware). The render stream's buffer is copied into a non-paged ring, and the capture stream reads from it.

**Why a cable and not a custom engine↔driver transport** (decision, replacing the IOCTL/shared-memory plan in the original draft):
- The engine already has verified WASAPI capture/loopback and render paths with drift compensation. With a cable, *App → dB-Deck* is "app plays into Virtual Input, engine loopback-captures Virtual Input", and *dB-Deck → OBS* is "engine renders a bus into Virtual Input, OBS records Virtual Output".
- No IOCTLs or user-mapped kernel memory means far less attack surface and fewer lifetime hazards.
- Cost: one extra shared-mode buffer of latency on each path (the Windows audio engine sits between the app and the driver anyway). Both endpoints share one clock, so there is no drift inside the cable.

## Answers to the original research questions
1. **Driver architecture:** PortCls + WaveRT miniport pairs (wave + topology filter per endpoint), root-enumerated virtual device. The KMDF miniport is used only for PnP/power as in the sample.
2. **Endpoint provisioning:** subdevices are registered in `StartDevice` from static `ENDPOINT_MINIPAIR` tables (`minipairs.h`).
3. **Dynamic endpoints:** possible via `PcRegisterSubdevice`/`PcUnregisterSubdevice` at runtime, but the endpoint builder caches endpoints and churn is fragile. **Plan:** a fixed pool of N cable pairs (for example 8) declared in the tables, each enabled or disabled through a registry-backed setting read at start. This is a driver-side limit, which the UI must state.
4. **PCM transfer:** apps ↔ WaveRT cyclic buffers (Windows audio engine). Driver timers advance positions and copy render → ring → capture.
5. **User/kernel communication:** none needed for audio (WASAPI). A pool-management interface (enable or rename slots) would be a small IOCTL set if added later.
6. **Naming and identity:** device description "dB-Deck Virtual Cable 1" (INF). Windows forms endpoint names from form factor + description. Endpoint IDs are stable for the root-enumerated device instance.
7. **Safe removal:** `devcon remove` (or Device Manager) tears down the subdevices. Apps receive `AUDCLNT_E_DEVICE_INVALIDATED`, which the engine already handles (bus → disconnected, retry).
8. **Signing and distribution:** development needs test signing in a VM. Distribution needs an EV certificate and attestation signing (Partner Center) or HLK. A test-signed package is not suitable for end users.
9. **Installation and uninstallation:** a separate, optional, elevated component (`pnputil /add-driver … /install` + devcon-style root device creation). The main app's uninstaller must warn when the cable is in use. Steps are in `driver/windows/README.md`.
10. **Failure recovery:** if no capture stream runs, render data is discarded. Capture underruns produce silence. Latency is bounded to 200 ms (oldest data dropped). Engine crashes don't affect the driver.

## Scaling (next)
- Pool of N cable pairs: duplicate the minipair tables per slot (macro-generated), give each slot its own ring, and enable slots through a registry value under the device's software key. The UI's "Create virtual device" then enables a slot and restarts the device.
- Per-slot friendly names via INF strings (static) or `KSPROPERTY_PIN_NAME`.
- Optional 32-bit float formats (the ring would need float↔int conversion without FPU use, or `KeSaveExtendedProcessorState`).

## Safety rules (unchanged)
Never enable test signing, disable Secure Boot, change boot configuration, install the driver, or reboot on the development workstation without explicit user approval. Prefer a disposable VM.
