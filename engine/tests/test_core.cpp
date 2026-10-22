#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "TestFramework.h"
#include "TestSignals.h"
#include "audio/AdaptiveStream.h"
#include "audio/Format.h"
#include "audio/Gain.h"
#include "audio/ToneGenerator.h"
#include "dsp/Effects.h"
#include "graph/UndoHistory.h"
#include "graph/AppMatch.h"
#include "graph/Validation.h"
#include "protocol/Protocol.h"
#include "util/StereoRing.h"

using namespace dbdeck;

// --- Ring buffer -------------------------------------------------------------

TEST("ring: push/pop preserves order and respects capacity") {
    StereoRing ring(64);
    CHECK(ring.capacity() == 64);
    std::vector<float> in(200 * 2), out(200 * 2);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i);
    CHECK(ring.push(in.data(), 200) == 64);  // full
    CHECK(ring.readable() == 64);
    CHECK(ring.pop(out.data(), 10) == 10);
    for (size_t i = 0; i < 20; ++i) CHECK(out[i] == in[i]);
    CHECK(ring.push(in.data(), 10) == 10);
    CHECK(ring.readable() == 64);
}

// --- Format conversion -------------------------------------------------------

TEST("format: int16 stereo round trip") {
    DeviceFormat f{48000, 2, SampleType::Int16, 2, 16, 0};
    std::vector<float> src = {0.5f, -0.5f, 1.5f, -1.5f};
    std::vector<int16_t> dev(4);
    stereoToDevice(src.data(), 2, f, dev.data());
    CHECK(dev[0] == 16384);
    CHECK(dev[1] == -16384);
    CHECK(dev[2] == 32767);   // clamped
    CHECK(dev[3] == -32768);
    std::vector<float> back(4);
    deviceToStereo(dev.data(), 2, f, back.data());
    CHECK_NEAR(back[0], 0.5, 1e-4);
    CHECK_NEAR(back[1], -0.5, 1e-4);
}

TEST("format: packed int24 round trip") {
    DeviceFormat f{48000, 2, SampleType::Int24, 3, 24, 0};
    std::vector<float> src = {0.25f, -0.75f};
    std::vector<uint8_t> dev(6);
    stereoToDevice(src.data(), 1, f, dev.data());
    std::vector<float> back(2);
    deviceToStereo(dev.data(), 1, f, back.data());
    CHECK_NEAR(back[0], 0.25, 1e-6);
    CHECK_NEAR(back[1], -0.75, 1e-6);
}

TEST("format: mono input is duplicated to both sides (dual mono)") {
    DeviceFormat f{48000, 1, SampleType::Float32, 4, 32, 0};
    float mono[3] = {0.1f, 0.2f, -0.3f};
    float st[6];
    deviceToStereo(mono, 3, f, st);
    CHECK(st[0] == 0.1f && st[1] == 0.1f);
    CHECK(st[4] == -0.3f && st[5] == -0.3f);
}

TEST("format: 5.1 fold-down mixes center at -3 dB and drops LFE") {
    DeviceFormat f{48000, 6, SampleType::Float32, 4, 32, 0x3F};
    float frame[6] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f};  // FL FR FC LFE BL BR
    float st[2];
    deviceToStereo(frame, 1, f, st);
    CHECK_NEAR(st[0], 0.7071, 1e-3);
    CHECK_NEAR(st[1], 0.7071, 1e-3);
}

TEST("format: stereo to 7.1 device fills front L/R only") {
    DeviceFormat f{48000, 8, SampleType::Float32, 4, 32, 0x63F};
    float st[2] = {0.3f, -0.4f};
    float dev[8];
    stereoToDevice(st, 1, f, dev);
    CHECK(dev[0] == 0.3f && dev[1] == -0.4f);
    for (int i = 2; i < 8; ++i) CHECK(dev[i] == 0.0f);
}

// --- Gain ----------------------------------------------------------------------

TEST("gain: dB conversions and fader taper") {
    CHECK_NEAR(dbToLinear(0), 1.0, 1e-6);
    CHECK_NEAR(dbToLinear(-6.0206f), 0.5, 1e-4);
    CHECK_NEAR(linearToDb(0.5f), -6.0206, 1e-3);
    CHECK(faderToGain(0.0f) == 0.0f);
    CHECK(faderToGain(1.0f) == 1.0f);
    CHECK_NEAR(linearToDb(faderToGain(0.5f)), -12.04, 0.01);
    float l, r;
    // Boost range above unity: linear in dB up to +20 dB at the top.
    CHECK_NEAR(linearToDb(faderToGain(kFaderMax)), 20.0, 0.01);
    CHECK_NEAR(linearToDb(faderToGain(1.25f)), 10.0, 0.01);
    CHECK(faderToGain(9.0f) == faderToGain(kFaderMax));
    for (float db : {-30.0f, -6.0f, 0.0f, 6.0f, 20.0f}) CHECK_NEAR(linearToDb(faderToGain(dbToFader(db))), db, 0.01);
    balanceGains(0.0f, l, r);
    CHECK(l == 1.0f && r == 1.0f);
    balanceGains(-1.0f, l, r);
    CHECK(l == 1.0f && r == 0.0f);
}

TEST("gain: smoothed gain ramps without discontinuity") {
    SmoothedParam g(0.0f);
    g.setTarget(1.0f);
    const float c = smoothingCoeff(5.0f, 48000);
    std::vector<float> buf(480 * 2, 1.0f);
    applySmoothedGain(buf.data(), 480, g, c);
    float maxStep = 0;
    for (size_t i = 1; i < 480; ++i) maxStep = std::max(maxStep, std::fabs(buf[i * 2] - buf[(i - 1) * 2]));
    CHECK(maxStep < 0.01f);  // no click-sized jumps
    CHECK(buf[0] < 0.01f);
    CHECK(buf[479 * 2] > 0.85f);
}

// --- Adaptive stream (clock domain bridge / resampler) ----------------------------

TEST("adaptive stream: 1:1 passes a tone with correct level and frequency") {
    AdaptiveStream s(48000, 48000, 960);
    ToneGenerator tone(48000, 1000, 0.5f);
    std::vector<float> in(480 * 2), out(480 * 2), all;
    for (int block = 0; block < 200; ++block) {
        tone.pull(in.data(), 480);
        s.write(in.data(), 480);
        s.pull(out.data(), 480);
        if (block > 20) all.insert(all.end(), out.begin(), out.end());
    }
    CHECK_NEAR(testsig::rms(all, 0), 0.5 / std::sqrt(2.0), 0.01);
    CHECK_NEAR(testsig::estimateFrequency(all, 0, 48000), 1000, 5);
    CHECK(s.stats().underruns == 0);
}

TEST("adaptive stream: converts 44.1 kHz to 48 kHz preserving pitch") {
    AdaptiveStream s(44100, 48000, 882);
    ToneGenerator tone(44100, 1000, 0.5f);
    std::vector<float> in(441 * 2), out(480 * 2), all;
    for (int block = 0; block < 300; ++block) {
        tone.pull(in.data(), 441);
        s.write(in.data(), 441);
        s.pull(out.data(), 480);
        if (block > 50) all.insert(all.end(), out.begin(), out.end());
    }
    CHECK_NEAR(testsig::estimateFrequency(all, 0, 48000), 1000, 5);
    CHECK_NEAR(testsig::rms(all, 0), 0.5 / std::sqrt(2.0), 0.02);
    CHECK(s.stats().underruns == 0);
}

TEST("adaptive stream: absorbs 0.1% clock drift without underruns") {
    // Producer clock runs 0.1% fast relative to consumer.
    AdaptiveStream s(48000, 48000, 960);
    ToneGenerator tone(48000, 500, 0.5f);
    std::vector<float> in(4096 * 2), out(480 * 2);
    double debt = 0;
    for (int block = 0; block < 6000; ++block) {  // 60 s of audio
        debt += 480 * 1.001;
        const size_t n = static_cast<size_t>(debt);
        debt -= n;
        tone.pull(in.data(), n);
        s.write(in.data(), n);
        s.pull(out.data(), 480);
    }
    auto st = s.stats();
    CHECK(st.overruns == 0);
    CHECK(st.underruns == 0);
    CHECK(st.ratio > 1.0005 && st.ratio < 1.0015);  // tracked the drift
    CHECK(st.fillFrames < 960 * 3);
}

TEST("adaptive stream: starvation yields silence then recovers") {
    AdaptiveStream s(48000, 48000, 480);
    std::vector<float> ones(480 * 2, 0.25f), out(480 * 2, 9.0f);
    s.pull(out.data(), 480);
    CHECK(testsig::peak(out) == 0.0f);  // priming: silence
    s.write(ones.data(), 480);
    s.write(ones.data(), 480);
    s.pull(out.data(), 480);
    s.pull(out.data(), 480);
    s.pull(out.data(), 480);  // runs dry mid-block
    CHECK(s.stats().underruns == 1);
    CHECK(s.stats().starving);
}

// --- Effects -----------------------------------------------------------------------

TEST("effects: gain effect applies dB gain") {
    auto e = createEffect(EffectType::Gain);
    CHECK(e->setParam("gainDb", -6.0206f));
    std::vector<float> buf(4800 * 2, 1.0f);
    e->process(buf.data(), 4096);
    e->process(buf.data() + 4096 * 2, 4800 - 4096);
    CHECK_NEAR(buf.back(), 0.5, 1e-3);
}

TEST("effects: EQ peaking boost raises level at center frequency only") {
    auto measure = [](float hz) {
        auto e = createEffect(EffectType::Eq);
        e->setParam("midFreq", 1000);
        e->setParam("midGainDb", 12);
        e->setParam("midQ", 1.0f);
        ToneGenerator t(48000, hz, 0.1f);
        std::vector<float> buf(4096 * 2), all;
        for (int i = 0; i < 12; ++i) {
            t.pull(buf.data(), 4096);
            e->process(buf.data(), 4096);
            if (i > 2) all.insert(all.end(), buf.begin(), buf.end());
        }
        return linearToDb(static_cast<float>(testsig::rms(all, 0) * std::sqrt(2.0) / 0.1));
    };
    CHECK_NEAR(measure(1000), 12.0, 0.5);
    CHECK_NEAR(measure(60), 0.0, 0.5);
}

TEST("effects: compressor reduces loud signal by expected ratio") {
    auto e = createEffect(EffectType::Compressor);
    e->setParam("thresholdDb", -20);
    e->setParam("ratio", 4);
    e->setParam("kneeDb", 0);
    e->setParam("attackMs", 1);
    std::vector<float> buf(4096 * 2, 0.5f);  // DC at -6 dBFS
    for (int i = 0; i < 10; ++i) {
        std::fill(buf.begin(), buf.end(), 0.5f);
        e->process(buf.data(), 4096);
    }
    // -6 dB input, 14 dB over threshold -> 3.5 dB over -> output -16.5 dB.
    CHECK_NEAR(linearToDb(buf.back()), -16.5, 0.3);
}

TEST("effects: limiter never exceeds ceiling") {
    auto e = createEffect(EffectType::Limiter);
    e->setParam("ceilingDb", -3);
    ToneGenerator t(48000, 200, 2.0f);  // +6 dBFS sine
    std::vector<float> buf(4096 * 2);
    float maxOut = 0;
    for (int i = 0; i < 10; ++i) {
        t.pull(buf.data(), 4096);
        e->process(buf.data(), 4096);
        maxOut = std::max(maxOut, testsig::peak(buf));
    }
    CHECK(maxOut <= dbToLinear(-3.0f) + 1e-6f);
}

TEST("effects: noise gate attenuates signal below threshold") {
    auto e = createEffect(EffectType::Gate);
    e->setParam("thresholdDb", -30);
    e->setParam("rangeDb", -60);
    e->setParam("holdMs", 0);
    e->setParam("releaseMs", 10);
    ToneGenerator quiet(48000, 300, 0.005f);  // -46 dBFS
    std::vector<float> buf(4096 * 2);
    for (int i = 0; i < 10; ++i) {
        quiet.pull(buf.data(), 4096);
        e->process(buf.data(), 4096);
    }
    CHECK(testsig::peak(buf) < 0.005f * dbToLinear(-50));
    ToneGenerator loud(48000, 300, 0.5f);
    for (int i = 0; i < 4; ++i) {
        loud.pull(buf.data(), 4096);
        e->process(buf.data(), 4096);
    }
    CHECK(testsig::peak(buf) > 0.45f);
}

TEST("effects: bypass crossfades and then passes audio unchanged") {
    auto e = createEffect(EffectType::Gain);
    e->setParam("gainDb", -20);
    e->setEnabled(false);
    std::vector<float> buf(4096 * 2, 1.0f);
    e->process(buf.data(), 4096);  // crossfade towards dry
    std::fill(buf.begin(), buf.end(), 1.0f);
    e->process(buf.data(), 4096);
    CHECK_NEAR(buf[100], 1.0, 1e-4);
}

TEST("effects: unknown parameters are rejected, values clamped") {
    auto e = createEffect(EffectType::Compressor);
    CHECK(!e->setParam("nope", 1));
    CHECK(e->setParam("ratio", 1000));
    CHECK(e->param("ratio") == 20.0f);
}

// --- App matching ------------------------------------------------------------------

TEST("app match: exe name match is case-insensitive and path-filterable") {
    AppMatchRule r = ruleForExe("Discord.exe");
    CHECK(matchesRule(r, {1, "discord.exe", "C:\\Users\\x\\AppData\\Local\\Discord\\app-1\\Discord.exe"}));
    CHECK(matchesRule(r, {2, "DISCORD.EXE", ""}));
    CHECK(!matchesRule(r, {3, "chrome.exe", ""}));
    r.pathContains = "discordptb";
    CHECK(!matchesRule(r, {1, "discord.exe", "C:\\Discord\\Discord.exe"}));
    CHECK(matchesRule(r, {1, "discord.exe", "C:\\DiscordPTB\\Discord.exe"}));
}

// --- Routing validation ------------------------------------------------------------

static ProfileConfig profileWithBuses(int extra) {
    ProfileConfig p = makeDefaultProfile("t");
    for (int i = 0; i < extra; ++i) {
        BusConfig b;
        b.id = "bus" + std::to_string(i);
        b.name = "Bus " + std::to_string(i);
        p.buses.push_back(b);
    }
    return p;
}

TEST("routing: bus order puts senders before receivers") {
    ProfileConfig p = profileWithBuses(2);
    p.findBus("bus0")->busSends.push_back({"bus1", true, 1.0f, false});
    p.findBus("bus1")->busSends.push_back({"main", true, 1.0f, false});
    BusOrder o = orderBuses(p);
    CHECK(o.cycle.empty());
    auto pos = [&](const std::string& id) { return std::find(o.order.begin(), o.order.end(), id) - o.order.begin(); };
    CHECK(pos("bus0") < pos("bus1"));
    CHECK(pos("bus1") < pos("main"));
}

TEST("routing: cycles are detected and rejected") {
    ProfileConfig p = profileWithBuses(2);
    p.findBus("bus0")->busSends.push_back({"bus1", true, 1.0f, false});
    p.findBus("bus1")->busSends.push_back({"bus0", true, 1.0f, false});
    auto issue = validateRouting(p);
    CHECK(issue.has_value());
    CHECK(issue->code == "cycle");
    // A disabled send does not form a cycle.
    p.findBus("bus1")->busSends[0].enabled = false;
    CHECK(!validateRouting(p).has_value());
}

TEST("routing: loopback of an output routed back to it is a feedback loop") {
    ProfileConfig p = profileWithBuses(1);
    p.mainBus()->deviceId = "{speakers}";
    ChannelConfig c = makeChannel(ChannelType::EndpointLoopback, "Cable", p);
    c.deviceId = "{speakers}";
    p.channels.push_back(c);
    auto issue = validateRouting(p);
    CHECK(issue && issue->code == "feedback");
    // Indirect: loopback -> bus0 -> main
    p.channels[0].sends[0].busId = "bus0";
    p.findBus("bus0")->busSends.push_back({"main", true, 1.0f, false});
    CHECK(validateRouting(p).has_value());
    // Different device is fine.
    p.channels[0].deviceId = "{cable}";
    CHECK(!validateRouting(p).has_value());
}

TEST("routing: recording side of a virtual cable routed into its playback side is feedback") {
    ProfileConfig p = profileWithBuses(1);
    p.findBus("bus0")->deviceId = "{cable-in}";  // bus plays into the cable
    ChannelConfig c = makeChannel(ChannelType::HardwareInput, "Cable out", p);
    c.deviceId = "{cable-out}";
    c.sends[0].busId = "bus0";
    p.channels.push_back(c);
    CHECK(!validateRouting(p).has_value());  // unknown pairing: allowed
    CablePairs pairs{{"{cable-out}", {"{cable-16ch}", "{cable-in}"}}};
    auto issue = validateRouting(p, pairs);
    CHECK(issue && issue->code == "feedback");
    p.channels[0].sends[0].busId = "main";  // Main plays elsewhere: fine
    CHECK(!validateRouting(p, pairs).has_value());
}

// --- Protocol ----------------------------------------------------------------------

TEST("protocol: parses valid requests and rejects malformed ones") {
    Request r = parseRequest(R"({"id":7,"cmd":"setChannelVolume","args":{"id":"x","volume":0.5}})");
    CHECK(r.id == 7);
    CHECK(r.cmd == "setChannelVolume");
    CHECK(argFloat(r.args, "volume", 0, 1) == 0.5f);
    CHECK_THROWS(argFloat(r.args, "volume", 0.6f, 1));
    CHECK_THROWS(argString(r.args, "missing"));
    CHECK_THROWS(argBool(r.args, "volume"));
    CHECK_THROWS(parseRequest("{not json"));
    CHECK_THROWS(parseRequest(R"({"cmd":"x"})"));
    CHECK_THROWS(parseRequest(R"({"id":1,"cmd":"x","args":[1]})"));
    CHECK_THROWS(parseRequest(R"([1,2])"));
    try {
        parseRequest("{bad");
    } catch (const ProtocolError& e) {
        CHECK(e.code == "bad_request");
    }
    json resp = errorResponse(3, "not_found", "nope");
    CHECK(resp["ok"] == false);
    CHECK(resp["error"]["code"] == "not_found");
}

// ---------------------------------------------------------------------------
// Undo history

TEST("undo: a fader drag is one step; different controls are separate steps") {
    using namespace std::chrono_literals;
    UndoHistory h;
    auto t = UndoHistory::Clock::time_point{} + 10s;
    int value = 0;
    auto snap = [&] { return nlohmann::json(value); };
    for (int i = 0; i < 20; ++i, t += 30ms) {  // 20 updates, 30 ms apart
        h.record(snap, "channel:a:volume", t);
        ++value;
    }
    CHECK(h.undoDepth() == 1);
    h.record(snap, "channel:b:volume", t);  // another fader
    ++value;
    CHECK(h.undoDepth() == 2);
    t += 2s;                                // same fader again, after a pause
    h.record(snap, "channel:b:volume", t);
    ++value;
    CHECK(h.undoDepth() == 3);
    h.record(snap, "", t);                  // discrete edits never merge
    h.record(snap, "", t);
    CHECK(h.undoDepth() == 5);
}

TEST("undo: undo and redo walk back and forth; a new edit clears redo") {
    using namespace std::chrono_literals;
    UndoHistory h;
    auto t = UndoHistory::Clock::time_point{} + 10s;
    nlohmann::json state = 0;
    auto edit = [&](int v) {
        h.record([&] { return state; }, "", t);
        state = v;
    };
    edit(1);
    edit(2);
    auto u = h.undo(state);
    CHECK(u && *u == 1);
    state = *u;
    CHECK(h.redoDepth() == 1);
    auto r = h.redo(state);
    CHECK(r && *r == 2);
    state = *r;
    CHECK(h.undoDepth() == 2 && h.redoDepth() == 0);
    state = *h.undo(state);
    edit(7);
    CHECK(h.redoDepth() == 0);
    CHECK(!h.redo(state));
    CHECK(*h.undo(state) == 1);
}

TEST("undo: a merged drag after an undo starts a new step") {
    using namespace std::chrono_literals;
    UndoHistory h;
    auto t = UndoHistory::Clock::time_point{} + 10s;
    nlohmann::json s = 0;
    h.record([&] { return s; }, "bus:x:volume", t);
    s = 1;
    s = *h.undo(s);
    h.record([&] { return s; }, "bus:x:volume", t + 10ms);
    CHECK(h.undoDepth() == 1);
    CHECK(h.redoDepth() == 0);
}

TEST("undo: history depth is capped") {
    UndoHistory h(3);
    for (int i = 0; i < 10; ++i) h.record([&] { return nlohmann::json(i); }, "", UndoHistory::Clock::now());
    CHECK(h.undoDepth() == 3);
}

TEST("model: structural view ignores levels, names and colors but not routing") {
    ProfileConfig p = makeDefaultProfile("x");
    ChannelConfig c = makeChannel(ChannelType::TestTone, "Tone", p);
    p.channels.push_back(c);
    const json base = structuralView(p);
    ProfileConfig q = p;
    q.channels[0].volume = 0.2f;
    q.channels[0].mute = true;
    q.channels[0].name = "Renamed";
    q.channels[0].color = "#ff0000";
    q.channels[0].sends[0].gain = 0.5f;
    q.buses[0].volume = 1.3f;
    q.name = "other";
    CHECK(structuralView(q) == base);
    ProfileConfig r = p;
    r.channels[0].sends[0].enabled = false;
    CHECK(structuralView(r) != base);
    ProfileConfig s = p;
    s.channels[0].deviceId = "dev";
    CHECK(structuralView(s) != base);
}
