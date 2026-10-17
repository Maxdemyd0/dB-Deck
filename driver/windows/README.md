# dB-Deck virtual audio driver

No driver code is in this repository yet. This is deliberate: the WDK was not available, and the driver must be developed and tested in an isolated VM with test signing, never on a primary workstation without explicit approval.

See [docs/DRIVER_PLAN.md](../../docs/DRIVER_PLAN.md) for the architecture (a SysVAD-derived WaveRT miniport, a slot pool, a shared-memory ring transport, IOCTL control), the staged plan and the signing requirements.
