// model.h - GUI-facing packet, key, node and receiver state.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mcs::gui {

struct ChannelKey {
    std::string label;
    std::vector<uint8_t> key;
    uint8_t hash = 0;
    bool builtin = false;
    bool enabled = true;
};

struct NodeRecord {
    std::array<uint8_t, 32> pubkey{};
    std::string pubkeyHex;
    std::string name;
    int nodeType = 0;
    bool hasLocation = false;
    double lat = 0.0;
    double lon = 0.0;
    uint32_t timestamp = 0;
    bool signatureValid = false;
    double firstSeen = 0.0;
    double lastSeen = 0.0;
    int count = 0;
};

struct PacketRecord {
    uint64_t seq = 0;
    double tUnix = 0.0;
    double freqHz = 0.0;
    uint8_t header = 0;
    uint8_t version = 0;
    uint8_t route = 0;
    uint8_t type = 0;
    int hops = 0;
    std::vector<uint8_t> path;
    uint8_t channelHash = 0;
    std::string channelLabel;
    bool macOk = false;
    int keyIndex = -1;
    double quality = 0.0;
    std::vector<uint8_t> payload;
    std::string rawHex;

    bool isAdvert = false;
    NodeRecord advert;

    bool isText = false;
    uint32_t msgTimestamp = 0;
    uint8_t msgFlags = 0;
    std::string sender;
    std::string text;

    bool isData = false;
    uint16_t dataType = 0;
    std::vector<uint8_t> data;

    bool isMeshCore = false;
};

struct ReceiverStatus {
    bool running = false;
    std::string source = "RTL-SDR";
    std::string device;
    std::string lastError;
    double freqHz = 0.0;
    uint32_t rate = 0;
    double noiseFloor = 0.0;
    double lastCaptureSec = 0.0;
    double lastDecodeMs = 0.0;
    unsigned long long triggers = 0;
    unsigned long long captures = 0;
    unsigned long long packets = 0;
    double progress = -1.0;  // WAV only: 0..1, <0 unknown
};

struct MapMarker {
    double lat = 0.0;
    double lon = 0.0;
    std::string id;
    std::string label;
    bool fresh = false;
    bool selected = false;
};

struct ReceiverConfig {
    bool live = true;
    int device = 0;
    double freqHz = 910525000.0;
    uint32_t rate = 250000;
    std::string gain = "auto";
    int ppm = 0;
    bool biasTee = false;
    double thresholdDb = 3.0;
    int sf = 7;
    double bw = 62500.0;
    std::string wavPath;
    bool decodeAll = true;
    bool realtime = false;
    bool verbose = false;
};

struct Filters {
    char text[64] = "";
    int type = -1;       // -1 = all
    int route = -1;      // -1 = all
    int channel = -1;    // -1 = all
    bool showRelays = true;

    void reset()
    {
        text[0] = '\0';
        type = -1;
        route = -1;
        channel = -1;
        showRelays = true;
    }
};

} // namespace mcs::gui
