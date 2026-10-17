// Audio verification of the mixing graph with generated PCM: real samples
// are pushed through the compiled graph and measured at the outputs.

#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "TestFramework.h"
#include "TestSignals.h"
#include "audio/ToneGenerator.h"
#include "graph/GraphCompiler.h"
#include "graph/Mixer.h"
#include "graph/Scenes.h"

using namespace dbdeck;

namespace {

struct Rig {
    ProfileConfig profile = makeDefaultProfile("test");
    RuntimeRegistry reg;
    Mixer mixer;
    std::map<std::string, std::shared_ptr<PullSource>> sources;     // channel id -> source
    std::map<std::string, std::shared_ptr<AdaptiveStream>> rings;   // bus id -> ring (non-direct)
    std::map<std::string, std::string> blocked;                     // channel id -> reason (all sends)

    Rig() {
        profile.mainBus()->deviceId = "dev-main";
        BusConfig stream;
        stream.id = "stream";
        stream.name = "Stream";
        stream.deviceId = "dev-stream";
        profile.buses.push_back(stream);
        rings["stream"] = std::make_shared<AdaptiveStream>(48000, 48000, 960);
    }

    ChannelConfig& addTone(const std::string& id, float hz, float amp) {
        ChannelConfig c = makeChannel(ChannelType::TestTone, id, profile);
        c.id = id;
        c.toneHz = hz;
        profile.channels.push_back(c);
        sources[id] = std::make_shared<ToneGenerator>(48000, hz, amp);
        return profile.channels.back();
    }

    void compile() {
        CompileContext ctx;
        ctx.sourcesFor = [&](const ChannelConfig& c) {
            std::vector<std::shared_ptr<PullSource>> v;
            if (auto it = sources.find(c.id); it != sources.end()) v.push_back(it->second);
            return v;
        };
        ctx.sendBlockedReason = [&](const ChannelConfig& c, const SendConfig&, const BusConfig&) {
            auto it = blocked.find(c.id);
            return it == blocked.end() ? std::string() : it->second;
        };
        ctx.outputFor = [&](const BusConfig& b) -> std::optional<OutputBinding> {
            if (b.deviceId.empty()) return std::nullopt;
            OutputBinding o;
            o.deviceId = b.deviceId;
            if (auto it = rings.find(b.id); it != rings.end()) o.ring = it->second;
            return o;
        };
        reg.prune(profile);
        reg.applyParams(profile);
        mixer.publish(compileGraph(profile, reg, ctx, mixer.nextEpoch()));
    }

    // Renders `blocks` x 480 frames; returns direct (Main) output after settling.
    std::vector<float> run(int blocks, std::vector<float>* streamOut = nullptr) {
        std::vector<float> direct(480 * 2), all, so(480 * 2);
        for (int i = 0; i < blocks; ++i) {
            mixer.process(direct.data(), 480);
            if (i >= blocks / 2) all.insert(all.end(), direct.begin(), direct.end());
            if (streamOut) {
                rings["stream"]->pull(so.data(), 480);
                if (i >= blocks / 2) streamOut->insert(streamOut->end(), so.begin(), so.end());
            }
        }
        return all;
    }
};

}  // namespace

TEST("mixer: two sources mix into Main with independent levels") {
    Rig r;
    r.addTone("a", 500, 0.4f);
    r.addTone("b", 1500, 0.4f);
    r.profile.findChannel("b")->volume = 0.5f;  // fader 0.5 -> 0.25 gain
    r.compile();
    auto out = r.run(100);
    CHECK_NEAR(testsig::toneAmplitude(out, 0, 48000, 500), 0.4, 0.01);
    CHECK_NEAR(testsig::toneAmplitude(out, 0, 48000, 1500), 0.1, 0.01);
}

TEST("mixer: new channels route to Main by default") {
    Rig r;
    auto& c = r.addTone("a", 700, 0.3f);
    CHECK(c.sends.size() == 1);
    CHECK(c.sends[0].busId == "main");
    CHECK(c.sends[0].enabled);
    r.compile();
    CHECK_NEAR(testsig::toneAmplitude(r.run(60), 0, 48000, 700), 0.3, 0.01);
}

TEST("mixer: buses are independent (per-bus sends)") {
    Rig r;
    r.addTone("game", 400, 0.4f);
    r.addTone("mic", 1200, 0.4f);
    // game -> Main 100%, Stream 65%; mic -> Stream only.
    r.profile.findChannel("game")->sends.push_back({"stream", true, 0.65f, false});
    auto* mic = r.profile.findChannel("mic");
    mic->sends[0].enabled = false;
    mic->sends.push_back({"stream", true, 1.0f, false});
    r.compile();
    std::vector<float> stream;
    auto main = r.run(200, &stream);
    CHECK_NEAR(testsig::toneAmplitude(main, 0, 48000, 400), 0.4, 0.01);
    CHECK(testsig::toneAmplitude(main, 0, 48000, 1200) < 0.001);
    CHECK_NEAR(testsig::toneAmplitude(stream, 0, 48000, 400), 0.26, 0.01);
    CHECK_NEAR(testsig::toneAmplitude(stream, 0, 48000, 1200), 0.4, 0.01);
}

TEST("mixer: mute silences channel smoothly; parameter change needs no rebuild") {
    Rig r;
    r.addTone("a", 500, 0.5f);
    r.compile();
    r.run(40);
    r.profile.findChannel("a")->mute = true;
    r.reg.applyParams(r.profile);  // no compile/publish
    std::vector<float> buf(480 * 2);
    r.mixer.process(buf.data(), 480);
    // First block after mute: ramps (no instantaneous step to zero).
    CHECK(std::fabs(buf[2]) > 0.0f || std::fabs(buf[4]) > 0.0f);
    auto out = r.run(40);
    CHECK(testsig::peak(out) < 1e-4f);
}

TEST("mixer: pre-fader send ignores channel fader") {
    Rig r;
    r.addTone("a", 600, 0.4f);
    auto* c = r.profile.findChannel("a");
    c->volume = 0.0f;
    c->sends.push_back({"stream", true, 1.0f, true});
    r.compile();
    std::vector<float> stream;
    auto main = r.run(200, &stream);
    CHECK(testsig::peak(main) < 1e-4f);
    CHECK_NEAR(testsig::toneAmplitude(stream, 0, 48000, 600), 0.4, 0.01);
}

TEST("mixer: group fader and group mute scale member channels") {
    Rig r;
    r.addTone("a", 500, 0.4f);
    GroupConfig g{"g1", "Games", 0.5f, false, false};
    r.profile.groups.push_back(g);
    r.profile.findChannel("a")->groupId = "g1";
    r.compile();
    CHECK_NEAR(testsig::toneAmplitude(r.run(60), 0, 48000, 500), 0.1, 0.01);
    r.profile.findGroup("g1")->mute = true;
    r.reg.applyParams(r.profile);
    CHECK(testsig::peak(r.run(60)) < 1e-4f);
}

TEST("mixer: bus-to-bus send sums post-fader bus output") {
    Rig r;
    r.addTone("a", 500, 0.2f);
    auto* c = r.profile.findChannel("a");
    c->sends[0].enabled = false;
    c->sends.push_back({"stream", true, 1.0f, false});
    r.profile.findBus("stream")->busSends.push_back({"main", true, 0.5f, false});
    r.compile();
    CHECK_NEAR(testsig::toneAmplitude(r.run(100), 0, 48000, 500), 0.1, 0.01);
}

TEST("mixer: outputs are clamped to full scale and clipping is counted") {
    Rig r;
    r.addTone("a", 300, 0.9f);
    r.addTone("b", 300, 0.9f);
    r.compile();
    auto out = r.run(60);
    CHECK(testsig::peak(out) <= 1.0f);
    MeterReading m = r.reg.findBus("main")->meter.drain();
    CHECK(m.clips > 0);
}

TEST("mixer: master mute silences every managed output") {
    Rig r;
    r.addTone("a", 500, 0.4f);
    r.profile.findChannel("a")->sends.push_back({"stream", true, 1.0f, false});
    r.compile();
    r.mixer.setMasterMute(true);
    std::vector<float> stream;
    auto main = r.run(200, &stream);
    CHECK(testsig::peak(main) < 1e-4f);
    CHECK(testsig::peak(stream) < 1e-4f);
}

TEST("mixer: blocked sends carry no audio (double-audio guard)") {
    Rig r;
    r.addTone("app", 500, 0.4f);
    r.blocked["app"] = "original audio already plays on this device";
    r.compile();
    CHECK(testsig::peak(r.run(40)) < 1e-6f);
}

TEST("mixer: graph swap while running picks up new routing and frees old graphs") {
    Rig r;
    r.mixer.setClockRunning(true);  // simulate an audio thread
    r.addTone("a", 500, 0.4f);
    r.compile();
    r.run(20);
    r.addTone("b", 1500, 0.4f);
    r.compile();  // published while "running"
    auto out = r.run(100);
    r.mixer.collectGarbage();
    CHECK_NEAR(testsig::toneAmplitude(out, 0, 48000, 1500), 0.4, 0.01);
    // Removing a channel while playing: its audio fades out, no crash.
    r.profile.channels.erase(r.profile.channels.begin());
    r.sources.erase("a");
    r.compile();
    out = r.run(100);
    CHECK(testsig::toneAmplitude(out, 0, 48000, 500) < 0.001);
    r.mixer.setClockRunning(false);
}

TEST("mixer: effects in a channel chain process the PCM") {
    Rig r;
    auto& c = r.addTone("a", 500, 0.4f);
    c.effects.push_back({"fx1", "gain", true, {{"gainDb", -6.0206f}}});
    r.compile();
    CHECK_NEAR(testsig::toneAmplitude(r.run(100), 0, 48000, 500), 0.2, 0.01);
}

TEST("mixer: balance pans channel") {
    Rig r;
    r.addTone("a", 500, 0.4f).balance = 1.0f;  // hard right
    r.compile();
    auto out = r.run(100);
    CHECK(testsig::toneAmplitude(out, 0, 48000, 500) < 0.001);
    CHECK_NEAR(testsig::toneAmplitude(out, 1, 48000, 500), 0.4, 0.01);
}

TEST("mixer: many channels and buses (no artificial limits)") {
    Rig r;
    for (int b = 0; b < 32; ++b) {
        BusConfig bus;
        bus.id = "b" + std::to_string(b);
        bus.name = bus.id;
        r.profile.buses.push_back(bus);
    }
    for (int i = 0; i < 64; ++i) {
        auto& c = r.addTone("c" + std::to_string(i), 100.0f + i * 10, 0.005f);
        for (int b = 0; b < 32; ++b) c.sends.push_back({"b" + std::to_string(b), true, 1.0f, false});
    }
    r.compile();
    auto out = r.run(20);
    CHECK(testsig::peak(out) > 0.0f);
    CHECK(r.mixer.stats().maxBlockUs < 10000.0);  // well inside a 10 ms block
}

TEST("scenes: capture and apply restore levels, mutes and sends") {
    ProfileConfig p = makeDefaultProfile("p");
    ChannelConfig c = makeChannel(ChannelType::TestTone, "a", p);
    c.volume = 0.8f;
    p.channels.push_back(c);
    SceneConfig s = captureScene(p, "s1", "Loud");
    p.channels[0].volume = 0.1f;
    p.channels[0].mute = true;
    p.channels[0].sends[0].gain = 0.2f;
    applyScene(p, s);
    CHECK_NEAR(p.channels[0].volume, 0.8, 1e-6);
    CHECK(!p.channels[0].mute);
    CHECK_NEAR(p.channels[0].sends[0].gain, 1.0, 1e-6);
}
