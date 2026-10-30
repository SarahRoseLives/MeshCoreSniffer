// MeshCoreSniffer - real-time RTL-SDR MeshCore decoder (default) with
// offline IQ-file decoding support.
//
//   MeshCoreSniffer [--freq 910.525] [--rate 1000000]
//                   [--gain auto|dB] [--ppm N] [--device N]
//                   [--bias-tee] [--key <hex|base64>] [--verbose]
//
//   MeshCoreSniffer --iq capture.wav [options]   # decode all packets
//   MeshCoreSniffer capture.wav [options]        # decode first packet
//
// Defaults match the MeshCore "USA" radio preset:
//   910.525 MHz, SF7, BW 62.5 kHz, CR 4/5, sync word 0x12.
//
// The default channel key is the MeshCore public channel key.

#include "crypto.h"
#include "lora.h"
#include "meshcore.h"
#include "rtl_sdr.h"
#include "wav.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Options {
    std::string wavPath;
    std::string iqPath;
    bool rtl = false;
    double freqHz = 910525000.0;
    uint32_t rate = 1000000;
    std::string gain = "auto";
    int ppm = 0;
    bool biasTee = false;
    int device = 0;
    int sf = 7;
    double bw = 62500.0;
    double thresholdDb = 3.0;
    bool verbose = false;
    bool help = false;
    std::vector<std::vector<uint8_t>> keys;
};

volatile std::sig_atomic_t g_stop = 0;

void handleSigInt(int) { g_stop = 1; }

std::string toHex(const std::vector<uint8_t>& v, bool spaced = false)
{
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * (spaced ? 3 : 2));
    for (size_t i = 0; i < v.size(); ++i) {
        if (spaced && i) s.push_back(' ');
        s.push_back(hex[v[i] >> 4]);
        s.push_back(hex[v[i] & 0x0F]);
    }
    return s;
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fromHex(const std::string& s, std::vector<uint8_t>& out)
{
    std::string t;
    for (char c : s) {
        if (c == ' ' || c == ':' || c == '-') continue;
        t.push_back(c);
    }
    if (t.size() % 2 != 0) return false;
    out.clear();
    out.reserve(t.size() / 2);
    for (size_t i = 0; i < t.size(); i += 2) {
        const int hi = hexVal(t[i]);
        const int lo = hexVal(t[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool fromBase64(const std::string& s, std::vector<uint8_t>& out)
{
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> t;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const size_t p = alphabet.find(c);
        if (p == std::string::npos) return false;
        t.push_back(static_cast<int>(p));
    }
    if (t.size() % 4 == 1) return false;
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;
    for (int v : t) {
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

bool parseKey(const std::string& text, std::vector<uint8_t>& key)
{
    if (fromHex(text, key) && (key.size() == 16 || key.size() == 32)) return true;
    if (fromBase64(text, key) && (key.size() == 16 || key.size() == 32)) return true;
    return false;
}

bool parseFreq(const std::string& s, double& hz)
{
    try {
        const double v = std::stod(s);
        if (v <= 0.0) return false;
        if (s.find('.') != std::string::npos || v < 10000.0)
            hz = v * 1e6; // MHz
        else
            hz = v; // Hz
        return true;
    } catch (...) {
        return false;
    }
}

std::string utcString(uint32_t timestamp)
{
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm* tm = std::gmtime(&t);
    if (!tm) return "(invalid)";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", tm);
    return buf;
}

std::string localTimeString()
{
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    if (!tm) return "--:--:--";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return buf;
}

uint64_t fnv1a(const uint8_t* data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void usage(const char* argv0)
{
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " [options]              # live RTL-SDR (default)\n"
        << "  " << argv0 << " --iq FILE [options]    # decode all packets in an IQ WAV\n"
        << "  " << argv0 << " FILE.wav [options]     # decode first packet in an IQ WAV\n"
        << "\n"
        << "Common options:\n"
        << "  --sf N          spreading factor (default 7)\n"
        << "  --bw HZ         LoRa bandwidth in Hz (default 62500)\n"
        << "  --key HEX|B64   add a channel key (16 or 32 bytes); repeatable\n"
        << "                  the public channel key is always included:\n"
        << "                  8b3387e9c5cdea6ac9e5edbaa115cd72\n"
        << "  --verbose       print extra diagnostics\n"
        << "\n"
        << "RTL-SDR (default):\n"
        << "  --freq VALUE    center frequency, e.g. 910.525 (MHz) or 910525000 (Hz)\n"
        << "                  default: 910.525 MHz (MeshCore US preset)\n"
        << "  --rate HZ       sample rate (default 1000000; must be an integer\n"
        << "                  multiple of --bw, e.g. 250000, 500000, 1000000)\n"
        << "  --gain auto|dB  tuner gain (default auto)\n"
        << "  --ppm N         crystal frequency correction\n"
        << "  --device N      RTL-SDR device index (default 0)\n"
        << "  --bias-tee      enable the bias tee\n"
        << "  --threshold DB  energy trigger above noise floor (default 3 dB;\n"
        << "                  lower = more sensitive, higher = fewer false triggers)\n"
        << "\n"
        << "IQ file input (decode every packet in a recording):\n"
        << "  --iq FILE       process an IQ WAV through the same trigger/decode\n"
        << "                  path as the live receiver and print all packets\n"
        << "\n"
        << "The WAV must be 16-bit stereo IQ (I=left, Q=right).\n";
}

bool parseArgs(int argc, char** argv, Options& opt)
{
    // Always keep the public channel so adding a private key does not disable
    // public-channel decoding.
    opt.keys.clear();
    opt.keys.push_back({0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                        0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72});

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rtl") {
            opt.rtl = true;
        } else if (a == "--iq" && i + 1 < argc) {
            opt.iqPath = argv[++i];
        } else if (a == "--freq" && i + 1 < argc) {
            if (!parseFreq(argv[++i], opt.freqHz)) {
                std::cerr << "invalid --freq\n";
                return false;
            }
        } else if (a == "--rate" && i + 1 < argc) {
            try {
                opt.rate = static_cast<uint32_t>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "invalid --rate\n";
                return false;
            }
        } else if (a == "--gain" && i + 1 < argc) {
            opt.gain = argv[++i];
        } else if (a == "--ppm" && i + 1 < argc) {
            opt.ppm = std::stoi(argv[++i]);
        } else if (a == "--device" && i + 1 < argc) {
            opt.device = std::stoi(argv[++i]);
        } else if (a == "--bias-tee") {
            opt.biasTee = true;
        } else if (a == "--threshold" && i + 1 < argc) {
            opt.thresholdDb = std::stod(argv[++i]);
        } else if (a == "--sf" && i + 1 < argc) {
            opt.sf = std::stoi(argv[++i]);
        } else if (a == "--bw" && i + 1 < argc) {
            opt.bw = std::stod(argv[++i]);
        } else if ((a == "--key" || a == "--channel-key") && i + 1 < argc) {
            std::vector<uint8_t> k;
            if (!parseKey(argv[++i], k)) {
                std::cerr << "invalid --key (expected 16/32-byte hex or base64)\n";
                return false;
            }
            if (std::find(opt.keys.begin(), opt.keys.end(), k) == opt.keys.end())
                opt.keys.push_back(std::move(k));
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            opt.help = true;
            return true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            return false;
        } else if (opt.wavPath.empty()) {
            opt.wavPath = a;
        } else {
            std::cerr << "unexpected argument: " << a << "\n";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Offline/file-mode pretty printer (the original detailed output).
// ---------------------------------------------------------------------------
int printFilePacket(const mc::LoraPacket& pkt, const Options& opt, double sampleRate)
{
    std::cout << "        preamble at " << std::fixed << std::setprecision(6)
              << (static_cast<double>(pkt.preambleStart) / sampleRate) << " s\n"
              << "Header: payload=" << pkt.header.payloadLen
              << " bytes, CR=4/" << (4 + pkt.header.cr)
              << ", CRC=" << (pkt.header.hasCrc ? "on" : "off") << "\n"
              << "CRC   : " << (pkt.crcOk ? "valid" : "not verified") << "\n";

    std::vector<uint8_t> meshBytes(pkt.data.begin(),
                                   pkt.data.begin() + pkt.header.payloadLen);

    std::string err;
    mc::MeshCorePacket mp;
    if (!mc::parseMeshCore(meshBytes, mp, err)) {
        std::cerr << "MeshCore parse error: " << err << "\n";
        std::cerr << "Raw LoRa payload: " << toHex(meshBytes, true) << "\n";
        return 1;
    }
    if (!mc::isMeshCoreV1(mp)) {
        std::cout << "Not a MeshCore v1 packet (header=0x" << std::hex
                  << static_cast<int>(mp.header) << ", version="
                  << static_cast<int>(mp.version) << ", type=0x"
                  << static_cast<int>(mp.type) << std::dec << ")\n"
                  << "Raw LoRa payload: " << toHex(meshBytes, true) << "\n";
        return 1;
    }

    std::cout << "MeshCore: header=0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(mp.header) << std::dec << std::setfill(' ')
              << "  version=" << static_cast<int>(mp.version)
              << "  route=" << mc::meshcoreRouteName(mp.route)
              << "  type=" << mc::meshcorePayloadName(mp.type) << "\n";

    if (mp.hasTransportCodes) {
        std::cout << "          transport codes: 0x" << std::hex
                  << mp.transportCodes[0] << ", 0x" << mp.transportCodes[1]
                  << std::dec << "\n";
    }

    std::cout << "          path_len=0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(mp.pathLenByte) << std::dec << std::setfill(' ')
              << " (" << mp.hashCount << " hop(s), " << mp.hashSize
              << "-byte hashes)";
    if (!mp.path.empty()) std::cout << "  path=" << toHex(mp.path);
    std::cout << "\n";

    std::cout << "Payload (" << mp.payload.size() << " bytes): "
              << toHex(mp.payload, true) << "\n\n";

    if (mp.type == 0x04) {
        mc::MeshCoreAdvert advert;
        if (!mc::parseAdvert(mp.payload, advert)) {
            std::cout << "ADVERT: malformed payload\n";
            return 1;
        }
        const std::vector<uint8_t> pub(advert.publicKey, advert.publicKey + 32);
        std::cout << "ADVERT : " << mc::meshcoreNodeTypeName(advert.nodeType)
                  << "  name=\"" << advert.name << "\"\n"
                  << "         pubkey=" << toHex(pub) << "\n"
                  << "         timestamp=" << advert.timestamp << " ("
                  << utcString(advert.timestamp) << ")\n";
        if (advert.hasLocation) {
            std::cout << "         location=" << std::fixed << std::setprecision(6)
                      << (advert.latitude / 1e6) << ", "
                      << (advert.longitude / 1e6) << "\n";
        }
        std::cout << "         signature=" << (advert.signatureValid ? "VALID" : "INVALID")
                  << "\n"
                  << "         signature-bytes=" << toHex(std::vector<uint8_t>(
                         advert.signature, advert.signature + 64), true) << "\n";
        return 0;
    }

    bool printed = false;
    for (size_t ki = 0; ki < opt.keys.size(); ++ki) {
        std::vector<uint8_t> plaintext;
        bool macOk = false;
        uint8_t expectedHash = 0;
        if (!mc::decryptGroupPayload(mp, opt.keys[ki], plaintext, macOk, expectedHash)) {
            if (opt.verbose && (mp.type == 0x05 || mp.type == 0x06)) {
                std::cout << "Key " << ki << ": no match (expected channel hash 0x"
                          << std::hex << static_cast<int>(expectedHash) << std::dec
                          << ")\n";
            }
            continue;
        }

        std::cout << "Channel: hash=0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(mp.payload[0]) << std::dec << std::setfill(' ')
                  << "  key=" << toHex(opt.keys[ki])
                  << "  MAC=" << (macOk ? "valid" : "INVALID") << "\n";

        if (mp.type == 0x05) {
            uint32_t timestamp = 0;
            uint8_t flags = 0;
            std::string text;
            if (mc::parseGroupText(plaintext, timestamp, flags, text)) {
                const int txtType = flags >> 2;
                const int attempt = flags & 0x03;
                std::cout << "Text   : timestamp=" << timestamp << " ("
                          << utcString(timestamp) << ")\n"
                          << "          flags=0x" << std::hex << std::setw(2)
                          << std::setfill('0') << static_cast<int>(flags) << std::dec
                          << std::setfill(' ') << " (txt_type=" << txtType
                          << ", attempt=" << attempt << ")\n"
                          << "Message: " << text << "\n";
            } else {
                std::cout << "Plaintext: " << toHex(plaintext, true) << "\n";
            }
        } else if (mp.type == 0x06) {
            uint16_t dataType = 0;
            std::vector<uint8_t> data;
            if (mc::parseGroupData(plaintext, dataType, data)) {
                std::cout << "Data   : type=0x" << std::hex << dataType << std::dec
                          << " len=" << data.size() << "\n"
                          << "         " << toHex(data, true) << "\n";
            } else {
                std::cout << "Plaintext: " << toHex(plaintext, true) << "\n";
            }
        }
        printed = true;
        break;
    }

    if (!printed) {
        if (mp.type == 0x05 || mp.type == 0x06) {
            std::cout << "Channel: no configured key matched; ciphertext not decrypted.\n";
            std::cout << "         Try --key <base64-or-hex> with the channel PSK.\n";
        } else {
            std::cout << "Payload is not a group text/data packet; no decryption attempted.\n";
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Real-time mode
// ---------------------------------------------------------------------------
void printRealtimePacket(const mc::LoraPacket& pkt, const Options& opt)
{
    std::vector<uint8_t> meshBytes(pkt.data.begin(),
                                   pkt.data.begin() + pkt.header.payloadLen);

    std::string err;
    mc::MeshCorePacket mp;
    const bool parsed = mc::parseMeshCore(meshBytes, mp, err);
    if (!parsed) {
        if (opt.verbose)
            std::cerr << "[rx] LoRa CRC valid but MeshCore parse failed: " << err << "\n";
        return;
    }
    if (!mc::isMeshCoreV1(mp)) {
        if (opt.verbose)
            std::cerr << "[rx] ignoring non-MeshCore v1 packet (header=0x"
                      << std::hex << static_cast<int>(mp.header) << std::dec
                      << ", version=" << static_cast<int>(mp.version)
                      << ", type=0x" << std::hex << static_cast<int>(mp.type)
                      << std::dec << ")\n";
        return;
    }

    // Deduplicate identical payloads from the same path.  Including the path
    // means the node's direct copy and the repeater's relay are both shown.
    std::vector<uint8_t> hashBytes = mp.payload;
    hashBytes.insert(hashBytes.end(), mp.path.begin(), mp.path.end());
    hashBytes.push_back(mp.pathLenByte);
    hashBytes.push_back(mp.route);
    const uint64_t h = fnv1a(hashBytes.data(), hashBytes.size());

    static std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> recent;
    const auto now = std::chrono::steady_clock::now();
    while (!recent.empty() && now - recent.front().second > std::chrono::seconds(2))
        recent.pop_front();
    for (const auto& r : recent) {
        if (r.first == h) return; // duplicate within 2 s
    }
    recent.push_back({h, now});

    std::cout << "[" << localTimeString() << "] "
              << std::fixed << std::setprecision(3) << (opt.freqHz / 1e6)
              << " MHz  ";

    const char* typeName = mc::meshcorePayloadName(mp.type);

    if (mp.type == 0x04) {
        mc::MeshCoreAdvert advert;
        if (mc::parseAdvert(mp.payload, advert)) {
            std::cout << "ADVERT  " << mc::meshcoreNodeTypeName(advert.nodeType)
                      << "  name=\"" << advert.name << "\"";
            if (advert.hasLocation) {
                std::cout << "  loc=" << std::fixed << std::setprecision(6)
                          << (advert.latitude / 1e6) << ","
                          << (advert.longitude / 1e6);
            }
            std::cout << "  sig=" << (advert.signatureValid ? "valid" : "INVALID");
            if (opt.verbose) {
                const std::vector<uint8_t> pub(advert.publicKey, advert.publicKey + 32);
                std::cout << "  key=" << toHex(pub) << "  ("
                          << utcString(advert.timestamp) << ")";
            }
            std::cout << "\n";
        } else {
            std::cout << "ADVERT  malformed\n";
        }
        return;
    }

    if (mp.type == 0x05 || mp.type == 0x06) {
        const int ch = mp.payload.empty() ? -1 : mp.payload[0];
        for (const auto& key : opt.keys) {
            std::vector<uint8_t> plaintext;
            bool macOk = false;
            uint8_t expectedHash = 0;
            if (!mc::decryptGroupPayload(mp, key, plaintext, macOk, expectedHash))
                continue;

            if (mp.type == 0x05) {
                uint32_t timestamp = 0;
                uint8_t flags = 0;
                std::string text;
                if (mc::parseGroupText(plaintext, timestamp, flags, text)) {
                    std::cout << "GRP_TXT  ch=0x" << std::hex << std::setw(2)
                              << std::setfill('0') << ch << std::dec << std::setfill(' ')
                              << "  hops=" << mp.hashCount
                              << "  \"" << text << "\"";
                    if (opt.verbose) std::cout << "  (" << utcString(timestamp) << ")";
                    std::cout << "\n";
                } else {
                    std::cout << "GRP_TXT  ch=0x" << std::hex << ch << std::dec
                              << "  plaintext=" << toHex(plaintext, true) << "\n";
                }
            } else {
                uint16_t dataType = 0;
                std::vector<uint8_t> data;
                if (mc::parseGroupData(plaintext, dataType, data)) {
                    std::cout << "GRP_DATA ch=0x" << std::hex << std::setw(2)
                              << std::setfill('0') << ch << std::dec << std::setfill(' ')
                              << "  hops=" << mp.hashCount
                              << "  type=0x" << std::hex << dataType << std::dec
                              << "  len=" << data.size() << "\n";
                } else {
                    std::cout << "GRP_DATA ch=0x" << std::hex << ch << std::dec << "\n";
                }
            }
            return;
        }
        std::cout << typeName << "  ch=0x" << std::hex << std::setw(2)
                  << std::setfill('0') << ch << std::dec << std::setfill(' ')
                  << "  (no matching key)\n";
        return;
    }

    std::cout << typeName << "  route=" << mc::meshcoreRouteName(mp.route)
              << "  hops=" << mp.hashCount << "  payload=" << mp.payload.size()
              << " bytes";
    if (opt.verbose) std::cout << "  " << toHex(mp.payload, true);
    std::cout << "\n";
}

double rmsBlock(const std::complex<double>* x, int n)
{
    if (n <= 0) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += std::norm(x[i]);
    return std::sqrt(s / n);
}

using SampleSource = std::function<int(std::vector<std::complex<double>>&, int, std::string&)>;

int runStream(const Options& opt, const mc::LoraParams& params, const SampleSource& source)
{
    const int blockSize = std::max(64, static_cast<int>(opt.rate / 1000)); // ~1 ms
    std::vector<std::complex<double>> chunk;
    chunk.reserve(65536);

    std::deque<std::complex<double>> preRoll;
    const size_t preRollMax = static_cast<size_t>(opt.rate / 5); // 0.2 s
    std::vector<std::complex<double>> capture;
    capture.reserve(static_cast<size_t>(opt.rate) * 2);

    bool active = false;
    size_t quietSamples = 0;
    const size_t quietLimit = static_cast<size_t>(opt.rate * 3 / 20); // 150 ms
    const size_t maxCapture = static_cast<size_t>(opt.rate) * 2;      // 2 s

    double noiseFloor = 0.02;
    bool noiseInit = false;
    const double triggerFactor = std::pow(10.0, opt.thresholdDb / 20.0);

    long long globalSamples = 0;
    while (!g_stop) {
        std::string err;
        const int rc = source(chunk, 65536, err);
        if (rc == 1) break; // EOF (simulation)
        if (rc != 0) {
            if (!g_stop && !err.empty()) std::cerr << err << "\n";
            break;
        }
        const int pairs = static_cast<int>(chunk.size());
        if (pairs <= 0) { globalSamples += 0; continue; }

        if (!active) {
            int triggerBlock = -1;
            for (int start = 0; start < pairs; start += blockSize) {
                const int n = std::min(blockSize, pairs - start);
                const double rms = rmsBlock(chunk.data() + start, n);
                if (!noiseInit) {
                    noiseFloor = std::max(0.005, rms);
                    noiseInit = true;
                }
                if (rms < noiseFloor * 2.0)
                    noiseFloor = noiseFloor * 0.995 + rms * 0.005;
                if (rms > std::max(0.005, noiseFloor * triggerFactor)) {
                    triggerBlock = start;
                    break;
                }
            }

            if (triggerBlock >= 0) {
                active = true;
                quietSamples = 0;
                capture.assign(preRoll.begin(), preRoll.end());
                const int endOfTrigger = std::min(pairs, triggerBlock + blockSize);
                capture.insert(capture.end(), chunk.begin(), chunk.begin() + endOfTrigger);
                capture.insert(capture.end(), chunk.begin() + endOfTrigger, chunk.end());
                preRoll.clear();
                if (opt.verbose)
                    std::cerr << "[rx] burst trigger at "
                              << std::fixed << std::setprecision(3)
                              << (static_cast<double>(globalSamples + triggerBlock) / opt.rate)
                              << " s (noise floor " << std::setprecision(4)
                              << noiseFloor << ")\n";
            } else {
                for (const auto& x : chunk) preRoll.push_back(x);
                while (preRoll.size() > preRollMax) preRoll.pop_front();
            }
        } else {
            capture.insert(capture.end(), chunk.begin(), chunk.end());
            for (int start = 0; start < pairs; start += blockSize) {
                const int n = std::min(blockSize, pairs - start);
                const double rms = rmsBlock(chunk.data() + start, n);
                if (rms < noiseFloor * 1.6)
                    quietSamples += static_cast<size_t>(n);
                else
                    quietSamples = 0;
            }

            if (quietSamples > quietLimit || capture.size() > maxCapture) {
                mc::LoraPacket pkt;
                std::string log;
                if (mc::decodeLora(capture, params, pkt, log, opt.verbose)) {
                    printRealtimePacket(pkt, opt);
                } else if (opt.verbose) {
                    std::cerr << "[rx] burst captured but no packet (" << log << ")\n";
                }
                active = false;
                quietSamples = 0;
                capture.clear();
            }
        }
        globalSamples += pairs;
    }

    std::cout << "\nStopped.\n";
    return 0;
}

int runRealtime(Options& opt)
{
    mc::RtlSdr sdr;
    std::string err;
    if (!sdr.open(opt.device, err)) {
        std::cerr << "RTL-SDR: " << err << "\n";
        return 1;
    }
    if (!sdr.configure(opt.freqHz, opt.rate, opt.gain, opt.ppm, opt.biasTee, err)) {
        std::cerr << "RTL-SDR: " << err << "\n";
        return 1;
    }

    mc::LoraParams params;
    params.sf = opt.sf;
    params.bw = opt.bw;
    params.fs = opt.rate;
    if (!mc::initLora(params, err)) {
        std::cerr << "LoRa parameter error: " << err << "\n"
                  << "Hint: sample rate must be an integer multiple of bandwidth "
                     "(e.g. --rate 1000000 --bw 62500).\n";
        return 1;
    }

    std::signal(SIGINT, handleSigInt);
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop = 1; return TRUE; }, TRUE);
#endif

    std::cout << "RTL-SDR: " << sdr.deviceName() << "\n"
              << "Center : " << std::fixed << std::setprecision(6)
              << (opt.freqHz / 1e6) << " MHz";
    if (sdr.actualCenterFreq() != 0)
        std::cout << " (tuned " << std::setprecision(6)
                  << (static_cast<double>(sdr.actualCenterFreq()) / 1e6) << " MHz)";
    std::cout << "\n"
              << "Rate   : " << opt.rate << " Hz (BW " << (opt.bw / 1000.0)
              << " kHz, oversampling " << params.os << ")\n"
              << "Gain   : " << opt.gain;
    if (opt.ppm) std::cout << "  ppm=" << opt.ppm;
    std::cout << "\n"
              << "LoRa   : SF" << params.sf << " CR4/5 sync 0x12\n"
              << "Trigger: " << std::fixed << std::setprecision(1)
              << opt.thresholdDb << " dB above noise\n"
              << "Keys   : " << opt.keys.size() << " channel key(s)\n"
              << "Listening for MeshCore packets... (Ctrl+C to stop)\n"
              << std::endl;

    std::vector<uint8_t> raw(65536 * 2);
    auto source = [&](std::vector<std::complex<double>>& chunk, int maxSamples,
                      std::string& readErr) -> int {
        const int wantPairs = std::min(65536, maxSamples);
        int nRead = 0;
        if (sdr.read(raw.data(), wantPairs * 2, nRead, readErr) != 0) return -1;
        const int pairs = nRead / 2;
        chunk.resize(pairs);
        for (int i = 0; i < pairs; ++i) {
            const double I = (static_cast<int>(raw[static_cast<size_t>(2 * i)]) - 127.5) / 128.0;
            const double Q = (static_cast<int>(raw[static_cast<size_t>(2 * i + 1)]) - 127.5) / 128.0;
            chunk[static_cast<size_t>(i)] = std::complex<double>(I, Q);
        }
        return 0;
    };

    return runStream(opt, params, source);
}

int runIqFile(Options& opt)
{
    mc::WavData wav;
    std::string err;
    if (!mc::readWav(opt.iqPath, wav, err)) {
        std::cerr << "WAV error: " << err << "\n";
        return 1;
    }

    opt.rate = static_cast<uint32_t>(std::llround(wav.sampleRate));
    mc::LoraParams params;
    params.sf = opt.sf;
    params.bw = opt.bw;
    params.fs = wav.sampleRate;
    if (!mc::initLora(params, err)) {
        std::cerr << "LoRa parameter error: " << err << "\n";
        return 1;
    }

    std::signal(SIGINT, handleSigInt);
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop = 1; return TRUE; }, TRUE);
#endif

    std::cout << "IQ file: " << opt.iqPath << "\n"
              << "Rate    : " << opt.rate << " Hz (BW " << (opt.bw / 1000.0)
              << " kHz, oversampling " << params.os << ")\n"
              << "LoRa    : SF" << params.sf << " CR4/5 sync 0x12\n"
              << "Keys    : " << opt.keys.size() << " channel key(s)\n"
              << "Streaming through real-time capture logic...\n"
              << std::endl;

    size_t pos = 0;
    auto source = [&](std::vector<std::complex<double>>& chunk, int maxSamples,
                      std::string&) -> int {
        if (pos >= wav.iq.size()) return 1;
        const size_t n = std::min<size_t>(static_cast<size_t>(maxSamples),
                                          wav.iq.size() - pos);
        chunk.assign(wav.iq.begin() + static_cast<std::ptrdiff_t>(pos),
                     wav.iq.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return 0;
    };

    return runStream(opt, params, source);
}

int runFile(Options& opt)
{
    mc::WavData wav;
    std::string err;
    if (!mc::readWav(opt.wavPath, wav, err)) {
        std::cerr << "WAV error: " << err << "\n";
        return 1;
    }

    mc::LoraParams params;
    params.sf = opt.sf;
    params.bw = opt.bw;
    params.fs = wav.sampleRate;
    if (!mc::initLora(params, err)) {
        std::cerr << "LoRa parameter error: " << err << "\n";
        return 1;
    }

    std::cout << "Input : " << opt.wavPath << "\n"
              << "        " << wav.sampleRate << " Hz, " << wav.channels
              << " channel(s), " << wav.iq.size() << " IQ samples\n"
              << "LoRa  : SF" << params.sf << ", BW " << (params.bw / 1000.0)
              << " kHz, CR 4/5, sync 0x12\n";

    mc::LoraPacket pkt;
    std::string log;
    if (!mc::decodeLora(wav.iq, params, pkt, log, opt.verbose)) {
        std::cerr << "Decode failed: " << log << "\n";
        return 1;
    }
    return printFilePacket(pkt, opt, wav.sampleRate);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (!mc::cryptoSelfTest()) {
        std::cerr << "internal crypto self-test failed\n";
        return 2;
    }

    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;
    if (opt.help) return 0;

    if (!opt.iqPath.empty()) {
        if (!opt.wavPath.empty()) {
            std::cerr << "--iq cannot be combined with a positional WAV path\n";
            return 2;
        }
        return runIqFile(opt);
    }

    if (!opt.wavPath.empty())
        return runFile(opt);

    // No file input: RTL-SDR live mode is the default.
    return runRealtime(opt);
}
