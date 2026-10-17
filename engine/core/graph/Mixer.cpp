#include "graph/Mixer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace dbdeck {

namespace {

inline void addSmoothed(float* dst, const float* src, size_t frames, SmoothedParam& g, float coeff) {
    if (g.settled()) {
        const float v = g.current();
        if (v == 0.0f) return;
        for (size_t i = 0; i < frames * 2; ++i) dst[i] += src[i] * v;
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        const float v = g.next(coeff);
        dst[i * 2] += src[i * 2] * v;
        dst[i * 2 + 1] += src[i * 2 + 1] * v;
    }
}

}  // namespace

void CompiledGraph::allocate() {
    const size_t n = kMaxBlockFrames * 2;
    chanBuf.assign(n, 0.0f);
    preBuf.assign(n, 0.0f);
    tmpBuf.assign(n, 0.0f);
    outBuf.assign(n, 0.0f);
    ramp.assign(kMaxBlockFrames, 0.0f);
    busBuf.assign(buses.size(), std::vector<float>(n, 0.0f));
    busPreBuf.assign(buses.size(), std::vector<float>(n, 0.0f));
}

Mixer::Mixer(double rate) : rate_(rate), coeff_(smoothingCoeff(5.0f, rate)) {}

Mixer::~Mixer() {
    // Clock threads must already be stopped by the owner.
    pending_.store(nullptr);
    current_ = nullptr;
    live_.clear();
}

void Mixer::publish(std::unique_ptr<CompiledGraph> graph) {
    CompiledGraph* raw = graph.get();
    live_.push_back(std::move(graph));
    if (!clockRunning_) {
        // No audio thread: adopt immediately.
        pending_.store(nullptr, std::memory_order_relaxed);
        current_ = raw;
        ackEpoch_.store(raw->epoch, std::memory_order_relaxed);
        collectGarbage();
        return;
    }
    CompiledGraph* superseded = pending_.exchange(raw, std::memory_order_acq_rel);
    if (superseded) {
        // Never picked up by the audio thread, safe to free right away.
        std::erase_if(live_, [&](const std::unique_ptr<CompiledGraph>& g) { return g.get() == superseded; });
    }
    collectGarbage();
}

void Mixer::collectGarbage() {
    const uint64_t ack = ackEpoch_.load(std::memory_order_acquire);
    CompiledGraph* pending = pending_.load(std::memory_order_acquire);
    std::erase_if(live_, [&](const std::unique_ptr<CompiledGraph>& g) {
        return g->epoch < ack && g.get() != pending;
    });
}

void Mixer::setClockRunning(bool running) {
    if (!running) {
        // Adopt anything still pending so the control thread's view is current.
        if (CompiledGraph* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            current_ = p;
            ackEpoch_.store(p->epoch, std::memory_order_release);
        }
        collectGarbage();
    }
    clockRunning_ = running;
}

MixerStats Mixer::stats() const {
    MixerStats s;
    s.blocks = statBlocks_.load(std::memory_order_relaxed);
    s.frames = statFrames_.load(std::memory_order_relaxed);
    s.lastBlockUs = statLastUs_.load(std::memory_order_relaxed);
    s.maxBlockUs = statMaxUs_.load(std::memory_order_relaxed);
    return s;
}

void Mixer::process(float* directOut, size_t frames) {
    if (CompiledGraph* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
        current_ = p;
        ackEpoch_.store(p->epoch, std::memory_order_release);
    }
    const auto t0 = std::chrono::steady_clock::now();
    CompiledGraph* g = current_;
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min(frames - done, kMaxBlockFrames);
        float* out = directOut ? directOut + done * 2 : nullptr;
        if (g) processChunk(*g, out, n);
        else if (out) std::memset(out, 0, n * 2 * sizeof(float));
        done += n;
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    statBlocks_.fetch_add(1, std::memory_order_relaxed);
    statFrames_.fetch_add(frames, std::memory_order_relaxed);
    statLastUs_.store(us, std::memory_order_relaxed);
    if (us > statMaxUs_.load(std::memory_order_relaxed)) statMaxUs_.store(us, std::memory_order_relaxed);
}

void Mixer::processChunk(CompiledGraph& g, float* directOut, size_t n) {
    const size_t samples = n * 2;
    const size_t bytes = samples * sizeof(float);
    for (auto& b : g.busBuf) std::memset(b.data(), 0, bytes);

    float* chan = g.chanBuf.data();
    float* pre = g.preBuf.data();
    float* tmp = g.tmpBuf.data();

    // Channels: pull -> effects -> (pre copy) -> fader/balance -> meter -> sends.
    for (auto& c : g.channels) {
        if (c.inputs.empty()) continue;
        c.inputs[0]->pull(chan, n);
        for (size_t k = 1; k < c.inputs.size(); ++k) {
            c.inputs[k]->pull(tmp, n);
            for (size_t i = 0; i < samples; ++i) chan[i] += tmp[i];
        }
        for (Effect* e : c.effects) e->process(chan, n);

        bool anyPre = false;
        for (auto& s : c.sends) anyPre |= s.preFader;
        if (anyPre) std::memcpy(pre, chan, bytes);

        ChannelRuntime& rt = *c.rt;
        if (rt.gain.settled() && rt.balance.settled()) {
            float l, r;
            balanceGains(rt.balance.current(), l, r);
            const float gl = rt.gain.current() * l, gr = rt.gain.current() * r;
            for (size_t i = 0; i < n; ++i) {
                chan[i * 2] *= gl;
                chan[i * 2 + 1] *= gr;
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                const float gv = rt.gain.next(coeff_);
                float l, r;
                balanceGains(rt.balance.next(coeff_), l, r);
                chan[i * 2] *= gv * l;
                chan[i * 2 + 1] *= gv * r;
            }
        }
        rt.meter.process(chan, n);

        for (auto& s : c.sends) addSmoothed(g.busBuf[s.bus].data(), s.preFader ? pre : chan, n, s.rt->gain, coeff_);
    }

    // Buses in topological order.
    for (size_t b = 0; b < g.buses.size(); ++b) {
        CompiledBus& bus = g.buses[b];
        float* buf = g.busBuf[b].data();
        for (auto& in : bus.inputs) {
            const float* src = in.preFader ? g.busPreBuf[in.fromBus].data() : g.busBuf[in.fromBus].data();
            addSmoothed(buf, src, n, in.rt->gain, coeff_);
        }
        for (Effect* e : bus.effects) e->process(buf, n);
        std::memcpy(g.busPreBuf[b].data(), buf, bytes);
        applySmoothedGain(buf, n, bus.rt->gain, coeff_);
        bus.rt->meter.process(buf, n);
    }

    // Master (Mute All Managed Audio) ramp, shared by every output.
    float* ramp = g.ramp.data();
    for (size_t i = 0; i < n; ++i) ramp[i] = master_.next(coeff_);

    bool wroteDirect = false;
    float* out = g.outBuf.data();
    for (auto& o : g.outputs) {
        std::memset(out, 0, bytes);
        for (uint32_t b : o.buses) {
            const float* src = g.busBuf[b].data();
            for (size_t i = 0; i < samples; ++i) out[i] += src[i];
        }
        for (size_t i = 0; i < n; ++i) {
            out[i * 2] = std::clamp(out[i * 2] * ramp[i], -1.0f, 1.0f);
            out[i * 2 + 1] = std::clamp(out[i * 2 + 1] * ramp[i], -1.0f, 1.0f);
        }
        if (o.ring) {
            o.ring->write(out, n);
        } else if (directOut && !wroteDirect) {
            std::memcpy(directOut, out, bytes);
            wroteDirect = true;
        }
    }
    if (directOut && !wroteDirect) std::memset(directOut, 0, bytes);
}

}  // namespace dbdeck
