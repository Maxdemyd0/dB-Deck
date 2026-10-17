# Virtual audio driver plan

**Status: not implemented.** This build contains no kernel code. The development machine has no Windows Driver Kit (WDK) installed (`Include\<ver>\km\portcls.h` is absent), so no driver could be compiled or tested. Creating a placeholder or mock device would violate the project rules, so none exists. The app works fully without a driver (session control, capture, loopback, routing to any endpoint) and reports `driver.status = "not-available"` honestly in the UI.

The plan below is based on Microsoft's SysVAD sample and the WDK documentation. It has **not** been validated on hardware.

## 1. Architecture
A PortCls miniport driver (WaveRT) derived from **SysVAD** (`audio/sysvad` in microsoft/Windows-driver-samples):
- Each dB-Deck endpoint pair is a WaveRT render miniport and a WaveRT capture miniport with a topology filter. Like SysVAD's "virtual" endpoints, they have no hardware DMA: the cyclic WaveRT buffer is serviced by a timer in the driver.
- **Playback endpoint** ("dB-Deck Game Input"): apps render into it. The driver copies samples out of the WaveRT buffer into a shared ring that the engine reads.
- **Recording endpoint** ("dB-Deck Stream Output"): the engine writes bus PCM into a shared ring, and the driver copies it into the capture WaveRT buffer that apps (OBS, Discord) read.

## 2. Endpoint provisioning
SysVAD creates its endpoints from static tables (`ENDPOINT_MINIPAIR`) during `StartDevice`. Planned approach: a pool of N pre-declared endpoint pairs, enabled and disabled at runtime with `PcRegisterSubdevice` / `PcUnregisterSubdevice` (or interface enable/disable). The UI's "create virtual device" then activates a slot and names it.

## 3. Dynamic endpoints
Truly arbitrary live creation is possible in principle by registering additional subdevices at runtime. However, the audio endpoint builder caches endpoints, and churn there is fragile. **Decision (provisional):** use a fixed maximum pool (for example 16 pairs) as the safe alternative, documented as a driver-side limit rather than a product limit. If that ships, it must be stated clearly in the UI.

## 4. PCM transport
- WaveRT position is reported from a high-resolution timer (as SysVAD does). The driver's DPC copies between the WaveRT buffer and a lock-free ring.
- Shared memory: the engine maps a section created by the driver (MDL locked into a user-mode mapping via an IOCTL), so samples move without a per-buffer IOCTL. Clocking: the driver's timer is a separate clock domain, so the engine bridges it with the existing `AdaptiveStream`. No new resampling code is needed.

## 5. User/kernel communication
A device interface (custom GUID) exposed by the driver, opened by the engine with `CreateFile`. IOCTLs: `QUERY_VERSION`, `ENABLE_ENDPOINT(slot, name, format)`, `DISABLE_ENDPOINT(slot)`, `MAP_RING(slot, direction)`, `UNMAP_RING`. All inputs are validated in the driver, and only the engine's session or user may open the interface (security descriptor in the INF).

## 6. Naming and identity
Friendly names are set through the endpoint property store (`PKEY_Device_FriendlyName`) when a slot is enabled. The slot index is stable, so the endpoint ID is stable across reboots, and profiles store the endpoint ID like any other device.

## 7. Safe removal
Disable the slot: stop the engine streams, unmap the ring, `PcUnregisterSubdevice`. Apps holding streams get `AUDCLNT_E_DEVICE_INVALIDATED`, which they must handle anyway.

## 8. Signing and distribution
- Development: test signing in an **isolated test VM** only. Enabling test signing on the primary workstation requires explicit user approval and is not planned.
- Distribution: an EV code-signing certificate, then HLK testing or attestation signing through the Microsoft Partner Center. A test-signed package is **not** suitable for end users.

## 9. Installation and uninstallation
A separate, optional installer component (the base app does not require it): `pnputil /add-driver dbdeck.inf /install` (elevated), and an uninstall path that disables all slots, then `pnputil /delete-driver`. The main uninstaller must warn when virtual endpoints are in use.

## 10. Failure recovery
- If the engine disconnects, the driver keeps the endpoints and plays silence (render data is discarded, capture is zero-filled). Apps never see errors from an engine crash.
- The driver never blocks in the audio path, and every IOCTL buffer is length-checked.
- The engine re-maps rings on reconnect.

## Staged plan
1. Install the WDK and build stock SysVAD in a test VM.
2. **Prove playback**: one render endpoint, with PCM delivered into the engine (`dbdeck-wintests` gains a tone-through-virtual-endpoint test).
3. **Prove recording**: one capture endpoint receives an engine bus.
4. Slot pool, naming and IOCTL management, plus UI (Virtual Devices page).
5. Signing pipeline and separate installer.

## Interim: third-party virtual cables
The Virtual Devices page detects likely virtual endpoints (VB-Audio, Voicemeeter, etc.) and offers "Capture as input" (loopback of the cable's playback side) and "New bus to it". This gives the same App → dB-Deck → Main and Mic+Game → Stream bus → OBS topologies today, with real PCM.
