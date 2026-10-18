#pragma once
// Imports a Voicemeeter (Standard / Banana / Potato) settings file
// ("Voicemeeter*_LastSettings.xml") into a dB-Deck profile.
//
// What maps:
//   hardware input strips  -> HardwareInput channels (device matched by name)
//   virtual input strips   -> EndpointLoopback channels on the matching
//                             "Voicemeeter ... Input" playback endpoints, so
//                             apps keep playing where they already do
//   A buses with a device  -> buses (the first becomes Main)
//   B buses (virtual out)  -> buses without a device; the user assigns a
//                             virtual cable, because Voicemeeter's own B
//                             outputs only carry audio while Voicemeeter runs
//   strip -> bus routing   -> sends; strip/bus dB levels and mutes
// Everything else (EQ, compressor, gate, reverb, VBAN, MIDI, ...) is reported
// in `notes` and not imported.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "graph/Model.h"

namespace dbdeck {

struct ImportDevice {
    std::string id;
    std::string name;
    bool render = true;
};

struct VoicemeeterImportResult {
    ProfileConfig profile;
    std::string edition;              // "Potato" | "Banana" | "Standard"
    std::vector<std::string> notes;   // human-readable, shown to the user
};

// `devices`: current Windows endpoints, used to resolve names to stable IDs.
// Unresolved devices keep their name (deviceName) so they reconnect by name
// once present. Throws ConfigError on unreadable input.
VoicemeeterImportResult importVoicemeeter(const std::string& xml, const std::vector<ImportDevice>& devices,
                                          const std::string& profileName);

}  // namespace dbdeck
