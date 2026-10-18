// Voicemeeter settings import. The fixture is a synthetic Potato file with the
// same structure Voicemeeter writes (trimmed to the elements the importer reads,
// plus decoys that must be ignored).

#include <algorithm>
#include <cmath>

#include "TestFramework.h"
#include "audio/Gain.h"
#include "config/VoicemeeterImport.h"
#include "graph/Validation.h"

using namespace dbdeck;

static const char* kPotato = R"VM(<?xml version="1.0" encoding="utf-8"?>
<VBAudioVoicemeeterSettings>
<VoiceMeeterDeviceConfiguration>
	<InputDev index='1' type='4' name="Microphone (Voicemod)" />
	<InputDev index='2' type='4' name="Line In (Realtek(R) Audio)" />
	<InputDev index='3' type='4' name="Microphone (NVIDIA Broadcast)" />
	<InputDev index='4' name="-" />
	<InputDev index='5' name="-" />
	<OutputDev index='1' type='4' name="Speakers (USBMIC1.)" />
	<OutputDev index='2' type='4' name="Speakers (Realtek(R) Audio)" />
	<OutputDev index='3' type='4' name="Headphones (WH202A)" />
	<OutputDev index='4' name="-" pOut1='0' />
	<OutputDev index='5' name="-" pOut1='0' />
</VoiceMeeterDeviceConfiguration>
<VoiceMeeterParameters>
	<LabelStrip1></LabelStrip1>
	<LabelVirtualStrip2>Music</LabelVirtualStrip2>
	<Strip index='1' mute='0' mono='0' EQon='0' busa='0' busa2='0' busa3='0' busa4='0' busa5='0' busb='1' busb2='1' busb3='0' dblevel='0.00' />
	<StripComp index='1' comprate='1.000' />
	<Strip index='1' sendR='0.00' />
	<Strip index='2' mute='0' mono='0' EQon='0' busa='0' busa2='0' busa3='0' busa4='0' busa5='0' busb='0' busb2='0' busb3='0' dblevel='-11.22' />
	<Strip index='3' mute='0' mono='0' EQon='0' busa='0' busa2='0' busa3='0' busa4='0' busa5='0' busb='0' busb2='0' busb3='1' dblevel='0.00' />
	<StripComp index='3' comprate='4.000' />
	<Strip index='4' mute='0' mono='0' EQon='0' busa='1' busa2='0' busa3='0' busa4='0' busa5='0' busb='1' busb2='0' busb3='0' dblevel='0.00' />
	<Strip index='5' mute='0' mono='0' EQon='0' busa='1' busa2='0' busa3='0' busa4='0' busa5='0' busb='1' busb2='0' busb3='0' dblevel='0.00' />
	<Strip index='6' mute='0' mono='0' EQon='0' busa='1' busa2='0' busa3='0' busa4='0' busa5='0' busb='0' busb2='1' busb3='0' dblevel='4.26' />
	<Strip index='7' mute='0' mono='0' EQon='0' busa='1' busa2='0' busa3='0' busa4='0' busa5='0' busb='1' busb2='0' busb3='0' dblevel='11.62' />
	<Strip index='8' mute='1' mono='0' EQon='0' busa='1' busa2='1' busa3='1' busa4='1' busa5='0' busb='0' busb2='0' busb3='0' dblevel='0.00' />
	<Bus index='1' mute='0' vaio='0' mono='0' EQon='0' dblevel='-1.21' />
	<Bus index='2' mute='0' vaio='0' mono='0' EQon='0' dblevel='0.00' />
	<Bus index='3' mute='0' vaio='0' mono='0' EQon='0' dblevel='0.00' />
	<Bus index='4' mute='0' vaio='0' mono='0' EQon='0' dblevel='-6.00' />
	<Bus index='5' mute='0' vaio='0' mono='0' EQon='0' dblevel='0.00' />
	<Bus index='6' mute='0' vaio='0' mono='0' EQon='0' dblevel='-0.55' />
	<Bus index='7' mute='1' vaio='0' mono='0' EQon='0' dblevel='0.00' />
	<Bus index='8' mute='0' vaio='0' mono='0' EQon='0' dblevel='3.00' />
	<Bus index='1' retR='0.00' />
</VoiceMeeterParameters>
<VoiceMeeterC>
	<Strip index='1' mute='1' busa='1' busb='0' dblevel='-60.00' />
</VoiceMeeterC>
</VBAudioVoicemeeterSettings>
)VM";

static std::vector<ImportDevice> devices() {
    return {
        {"{in-voicemod}", "Microphone (Voicemod)", false},
        {"{in-line}", "Line In (Realtek(R) Audio)", false},
        {"{in-nv}", "Microphone (NVIDIA Broadcast)", false},
        {"{out-usb}", "Speakers (USBMIC1.)", true},
        {"{out-rt}", "Speakers (Realtek(R) Audio)", true},
        // WH202A headphones not connected.
        {"{vaio}", "Voicemeeter Input (VB-Audio Voicemeeter VAIO)", true},
        {"{aux}", "Voicemeeter AUX Input (VB-Audio Voicemeeter VAIO)", true},
        {"{vaio3}", "Voicemeeter VAIO3 Input (VB-Audio Voicemeeter VAIO)", true},
        {"{in1}", "Voicemeeter In 1 (VB-Audio Voicemeeter VAIO)", true},
    };
}

static const ChannelConfig* byDevice(const ProfileConfig& p, const std::string& id) {
    for (auto& c : p.channels)
        if (c.deviceId == id) return &c;
    return nullptr;
}

static bool sendsTo(const ProfileConfig& p, const ChannelConfig& c, const std::string& busName) {
    for (auto& s : c.sends) {
        const BusConfig* b = p.findBus(s.busId);
        if (b && b->name == busName && s.enabled) return true;
    }
    return false;
}

TEST("import: Voicemeeter Potato routing becomes a dB-Deck profile") {
    auto r = importVoicemeeter(kPotato, devices(), "From Voicemeeter");
    const ProfileConfig& p = r.profile;
    CHECK(r.edition == "Potato");

    // A1 -> Main, A2/A3 -> buses, A4/A5 (no device) skipped; B1..B3 used -> buses.
    const BusConfig* main = p.mainBus();
    CHECK(main && main->deviceId == "{out-usb}");
    CHECK_NEAR(20.0 * std::log10(static_cast<double>(main->volume) * main->volume), -1.21, 0.05);
    CHECK(p.buses.size() == 6);
    const BusConfig* a3 = nullptr;
    for (auto& b : p.buses)
        if (b.name == "A3") a3 = &b;
    CHECK(a3 && a3->deviceId.empty() && a3->deviceName == "Headphones (WH202A)");  // reconnects by name later

    // Hardware strips.
    const ChannelConfig* voicemod = byDevice(p, "{in-voicemod}");
    CHECK(voicemod && voicemod->type == ChannelType::HardwareInput);
    CHECK(sendsTo(p, *voicemod, "B1 (virtual mic)") && sendsTo(p, *voicemod, "B2 (virtual mic)"));
    CHECK(!sendsTo(p, *voicemod, "Main"));
    CHECK(byDevice(p, "{in-line}") == nullptr);  // not routed anywhere: skipped
    const ChannelConfig* nv = byDevice(p, "{in-nv}");
    CHECK(nv && nv->sends.size() == 1 && sendsTo(p, *nv, "B3 (virtual mic)"));

    // Virtual strips -> loopback of the VAIO endpoints, with gains and labels.
    const ChannelConfig* vaio = byDevice(p, "{vaio}");
    CHECK(vaio && vaio->type == ChannelType::EndpointLoopback);
    CHECK_NEAR(vaio->gainDb, 4.26, 0.01);
    const ChannelConfig* aux = byDevice(p, "{aux}");
    CHECK(aux && aux->name == "Music");
    CHECK_NEAR(aux->gainDb, 11.62, 0.01);
    CHECK(sendsTo(p, *aux, "Main") && sendsTo(p, *aux, "B1 (virtual mic)"));
    const ChannelConfig* vaio3 = byDevice(p, "{vaio3}");
    CHECK(vaio3 && vaio3->mute);
    CHECK(sendsTo(p, *vaio3, "Main") && sendsTo(p, *vaio3, "A2") && sendsTo(p, *vaio3, "A3"));

    // Bus mutes/levels; boosts above 0 dB reported.
    for (auto& b : p.buses)
        if (b.name == "B2 (virtual mic)") CHECK(b.mute);
    auto has = [&](const char* s) {
        return std::any_of(r.notes.begin(), r.notes.end(), [&](const std::string& n) { return n.find(s) != std::string::npos; });
    };
    for (auto& b : p.buses)
        if (b.name == "B3 (virtual mic)") CHECK_NEAR(20.0 * std::log10(faderToGain(b.volume)), 3.0, 0.01);  // boost kept
    CHECK(has("compressor"));
    CHECK(has("A4, which has no device"));
    CHECK(has("Close Voicemeeter"));
    CHECK(!validateRouting(p).has_value());
}

TEST("import: rejects files that are not Voicemeeter settings") {
    CHECK_THROWS(importVoicemeeter("<html></html>", devices(), "x"));
    CHECK_THROWS(importVoicemeeter("<VBAudioVoicemeeterSettings></VBAudioVoicemeeterSettings>", devices(), "x"));
}
