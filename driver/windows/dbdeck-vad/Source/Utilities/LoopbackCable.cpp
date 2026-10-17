/*++

dB-Deck Virtual Audio Driver - loopback cable implementation.

Copyright (c) dB-Deck contributors. Licensed under the Microsoft Public
License (MS-PL). See LoopbackCable.h.

--*/

#include "LoopbackCable.h"

#define LOOPBACK_POOLTAG          'kcBD'
#define LOOPBACK_RING_FRAMES      (48000 * 2)   // 2 s at 48 kHz, stereo frames
#define LOOPBACK_PREFILL_FRAMES   (48000 / 100) // 10 ms of jitter headroom (stream timers tick every 1 ms)
#define LOOPBACK_MAX_FILL_FRAMES  (48000 / 5)   // bound latency to 200 ms

namespace
{
    struct LoopbackRing
    {
        KSPIN_LOCK  Lock;
        LONG*       Samples;      // interleaved stereo, 32-bit signed
        ULONG       ReadFrame;
        ULONG       WriteFrame;
        ULONG       FillFrames;
        BOOLEAN     CaptureRunning;
        ULONG64     Overruns;
        ULONG64     Underruns;
    };

    LoopbackRing g_Ring;

    // Integer PCM only: the driver's data ranges expose 16- and 32-bit PCM,
    // and float formats are rejected by the callers (no FPU use in kernel).
    inline LONG ReadSample(const BYTE* p, USHORT bits)
    {
        switch (bits)
        {
        case 16: return ((LONG)*(const SHORT*)p) << 16;
        case 24: return (LONG)(((ULONG)p[0] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 24));
        case 32: return *(const LONG*)p;
        default: return 0;
        }
    }

    inline VOID WriteSample(BYTE* p, LONG v, USHORT bits)
    {
        switch (bits)
        {
        case 16: *(SHORT*)p = (SHORT)(v >> 16); break;
        case 24: p[0] = (BYTE)(v >> 8); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 24); break;
        case 32: *(LONG*)p = v; break;
        default: break;
        }
    }

    inline BOOLEAN IsFloatFormat(const WAVEFORMATEX* f)
    {
        if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            return TRUE;
        }
        if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        {
            const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)f;
            return IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? TRUE : FALSE;
        }
        return FALSE;
    }

    inline USHORT ContainerBits(const WAVEFORMATEX* f)
    {
        return f->nChannels ? (USHORT)((f->nBlockAlign / f->nChannels) * 8) : 0;
    }
}

#pragma code_seg("PAGE")
NTSTATUS LoopbackCable_Initialize()
{
    PAGED_CODE();
    RtlZeroMemory(&g_Ring, sizeof(g_Ring));
    KeInitializeSpinLock(&g_Ring.Lock);
    g_Ring.Samples = (LONG*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                            (SIZE_T)LOOPBACK_RING_FRAMES * 2 * sizeof(LONG),
                                            LOOPBACK_POOLTAG);
    return g_Ring.Samples ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID LoopbackCable_Cleanup()
{
    PAGED_CODE();
    if (g_Ring.Samples)
    {
        ExFreePoolWithTag(g_Ring.Samples, LOOPBACK_POOLTAG);
        g_Ring.Samples = NULL;
    }
}

#pragma code_seg()
VOID LoopbackCable_SetCaptureRunning(_In_ BOOLEAN Running)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Ring.Lock, &irql);
    g_Ring.CaptureRunning = Running;
    g_Ring.ReadFrame = 0;
    g_Ring.WriteFrame = 0;
    g_Ring.FillFrames = 0;
    if (Running && g_Ring.Samples)
    {
        // Start with a little silence so render/capture timer jitter never
        // starves the capture side.
        RtlZeroMemory(g_Ring.Samples, (SIZE_T)LOOPBACK_PREFILL_FRAMES * 2 * sizeof(LONG));
        g_Ring.WriteFrame = LOOPBACK_PREFILL_FRAMES;
        g_Ring.FillFrames = LOOPBACK_PREFILL_FRAMES;
    }
    KeReleaseSpinLock(&g_Ring.Lock, irql);
}

VOID LoopbackCable_WriteRender(_In_reads_bytes_(Bytes) const BYTE* Data, _In_ ULONG Bytes, _In_ const WAVEFORMATEX* Format)
{
    if (!Data || !Format || Format->nBlockAlign == 0 || Format->nChannels == 0)
    {
        return;
    }
    if (IsFloatFormat(Format))
    {
        return;
    }
    const USHORT bits = ContainerBits(Format);
    const ULONG frames = Bytes / Format->nBlockAlign;
    const ULONG bytesPerSample = bits / 8;

    KIRQL irql;
    KeAcquireSpinLock(&g_Ring.Lock, &irql);
    if (g_Ring.CaptureRunning && g_Ring.Samples)
    {
        for (ULONG i = 0; i < frames; ++i)
        {
            if (g_Ring.FillFrames >= LOOPBACK_MAX_FILL_FRAMES)
            {
                // Capture side is not keeping up: drop the oldest frame so
                // latency stays bounded.
                g_Ring.ReadFrame = (g_Ring.ReadFrame + 1) % LOOPBACK_RING_FRAMES;
                g_Ring.FillFrames--;
                g_Ring.Overruns++;
            }
            const BYTE* frame = Data + (SIZE_T)i * Format->nBlockAlign;
            const LONG l = ReadSample(frame, bits);
            const LONG r = Format->nChannels > 1 ? ReadSample(frame + bytesPerSample, bits) : l;
            LONG* dst = g_Ring.Samples + (SIZE_T)g_Ring.WriteFrame * 2;
            dst[0] = l;
            dst[1] = r;
            g_Ring.WriteFrame = (g_Ring.WriteFrame + 1) % LOOPBACK_RING_FRAMES;
            g_Ring.FillFrames++;
        }
    }
    KeReleaseSpinLock(&g_Ring.Lock, irql);
}

VOID LoopbackCable_ReadCapture(_Out_writes_bytes_(Bytes) BYTE* Data, _In_ ULONG Bytes, _In_ const WAVEFORMATEX* Format)
{
    if (!Data)
    {
        return;
    }
    RtlZeroMemory(Data, Bytes);
    if (!Format || Format->nBlockAlign == 0 || Format->nChannels == 0 || IsFloatFormat(Format))
    {
        return;
    }
    const USHORT bits = ContainerBits(Format);
    const ULONG frames = Bytes / Format->nBlockAlign;
    const ULONG bytesPerSample = bits / 8;

    KIRQL irql;
    KeAcquireSpinLock(&g_Ring.Lock, &irql);
    if (g_Ring.Samples)
    {
        for (ULONG i = 0; i < frames; ++i)
        {
            if (g_Ring.FillFrames == 0)
            {
                g_Ring.Underruns++;
                break;  // remaining frames stay silent
            }
            const LONG* src = g_Ring.Samples + (SIZE_T)g_Ring.ReadFrame * 2;
            BYTE* frame = Data + (SIZE_T)i * Format->nBlockAlign;
            WriteSample(frame, src[0], bits);
            if (Format->nChannels > 1)
            {
                WriteSample(frame + bytesPerSample, src[1], bits);
            }
            g_Ring.ReadFrame = (g_Ring.ReadFrame + 1) % LOOPBACK_RING_FRAMES;
            g_Ring.FillFrames--;
        }
    }
    KeReleaseSpinLock(&g_Ring.Lock, irql);
}
