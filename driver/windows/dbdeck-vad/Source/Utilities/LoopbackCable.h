/*++

dB-Deck Virtual Audio Driver - loopback cable.

Copyright (c) dB-Deck contributors. Licensed under the Microsoft Public
License (MS-PL), as part of a work derived from the Microsoft
SimpleAudioSample (see LICENSE-MS-PL.txt).

Abstract:

    Connects the driver's render endpoint ("dB-Deck Virtual Input") to its
    capture endpoint ("dB-Deck Virtual Output"). Whatever an application
    plays into the render endpoint can be recorded from the capture endpoint.
    The dB-Deck engine uses the endpoints through ordinary WASAPI, so no
    custom user/kernel interface exists.

    Samples are stored as 32-bit signed stereo in a non-paged ring protected
    by a spin lock. Both streams are paced by the same QPC-based timers, so
    there is no clock drift between them; the ring only absorbs scheduling
    jitter. Render data is discarded while no capture stream is running.

    All functions may be called at IRQL <= DISPATCH_LEVEL.

--*/

#pragma once

#include <portcls.h>
#include <ksmedia.h>

NTSTATUS LoopbackCable_Initialize();
VOID     LoopbackCable_Cleanup();

// Render stream: consume `Bytes` bytes of DMA buffer data in `Format`.
VOID LoopbackCable_WriteRender(_In_reads_bytes_(Bytes) const BYTE* Data, _In_ ULONG Bytes, _In_ const WAVEFORMATEX* Format);

// Capture stream: produce `Bytes` bytes of DMA buffer data in `Format`.
// Underruns are filled with silence.
VOID LoopbackCable_ReadCapture(_Out_writes_bytes_(Bytes) BYTE* Data, _In_ ULONG Bytes, _In_ const WAVEFORMATEX* Format);

// Called when the capture stream enters/leaves KSSTATE_RUN.
VOID LoopbackCable_SetCaptureRunning(_In_ BOOLEAN Running);
