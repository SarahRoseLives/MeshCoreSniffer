// lora.h - LoRa PHY demodulator + FEC (SF7..SF12, explicit header).
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

struct LoraParams {
    int sf = 7;
    double bw = 62500.0;
    double fs = 250000.0;
    bool ldro = false;

    int N = 0;   // 1 << sf
    int os = 0;  // fs / bw (integer oversampling)
    int sps = 0; // N * os
};

struct LoraHeader {
    int payloadLen = 0;
    int cr = 0;      // 1..4 -> 4/5 .. 4/8
    int hasCrc = 0;
};

struct LoraPacket {
    LoraHeader header;
    std::vector<uint8_t> data; // dewhitened payload + (unwhitened) LoRa CRC bytes
    bool crcOk = false;
    double quality = 0.0;
    int preambleStart = 0; // full-rate sample of the detected preamble symbol
    int headerPos = 0;     // decimated sample offset of the PHY header
    int payloadPos = 0;    // decimated sample offset of the payload
};

bool initLora(LoraParams& p, std::string& err);

// Decode the first valid LoRa packet in the recording.
bool decodeLora(const std::vector<std::complex<double>>& iq,
                const LoraParams& p,
                LoraPacket& out,
                std::string& log,
                bool verbose = false);

} // namespace mc
