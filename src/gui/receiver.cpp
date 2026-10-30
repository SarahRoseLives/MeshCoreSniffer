// receiver.cpp - GUI receiver thread: RTL-SDR or streaming IQ WAV.
#include "receiver.h"

#include "crypto.h"
#include "lora.h"
#include "meshcore.h"
#include "rtl_sdr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace mcs::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;

double nowUnix()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string hexBytes(const uint8_t* d, size_t n)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(k[d[i] >> 4]);
        s.push_back(k[d[i] & 0xf]);
    }
    return s;
}

std::string hexBytes(const std::vector<uint8_t>& v) { return hexBytes(v.data(), v.size()); }

uint64_t fnv1a(const uint8_t* data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// In-channel energy detector: DC blocker + one-pole low-pass with equivalent
// noise bandwidth equal to the LoRa bandwidth.  O(1) per sample.
struct BandDetector {
    std::complex<double> prev = 0.0;
    std::complex<double> hp = 0.0;
    std::complex<double> lp = 0.0;
    double hpCoeff = 0.99;
    double lpCoeff = 0.1;

    void init(double fs, double bw)
    {
        if (fs <= 0.0 || bw <= 0.0) return;
        const double hpHz = std::max(500.0, bw / 64.0);
        hpCoeff = std::exp(-2.0 * kPi * hpHz / fs);
        const double r = std::min(0.5, bw / fs);
        lpCoeff = 2.0 * r / (1.0 + r);
    }

    double process(std::complex<double> x)
    {
        const std::complex<double> blocked = x - prev + hpCoeff * hp;
        prev = x;
        hp = blocked;
        lp += lpCoeff * (blocked - lp);
        return std::norm(lp);
    }
};

// Streaming 16-bit PCM stereo/mono WAV reader.  Unlike mc::readWav it does not
// load multi-gigabyte captures into memory.
class WavStream {
public:
    bool open(const std::string& path, std::string& err)
    {
        f_.open(path, std::ios::binary);
        if (!f_) { err = "cannot open " + path; return false; }
        char hdr[12];
        f_.read(hdr, 12);
        if (!f_ || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
            err = "not a RIFF/WAVE file";
            return false;
        }
        bool haveFmt = false;
        while (f_) {
            char ch[8];
            f_.read(ch, 8);
            if (!f_) break;
            const uint32_t size = rd32(reinterpret_cast<const uint8_t*>(ch + 4));
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                std::vector<uint8_t> fmt(size);
                f_.read(reinterpret_cast<char*>(fmt.data()), size);
                if (!f_ || size < 16) { err = "bad fmt chunk"; return false; }
                audioFormat_ = rd16(&fmt[0]);
                channels_ = rd16(&fmt[2]);
                rate_ = rd32(&fmt[4]);
                bits_ = rd16(&fmt[14]);
                haveFmt = true;
                if (size & 1) f_.seekg(1, std::ios::cur);
            } else if (std::memcmp(ch, "data", 4) == 0) {
                if (!haveFmt) { err = "data before fmt"; return false; }
                if (audioFormat_ != 1 || bits_ != 16 || (channels_ != 1 && channels_ != 2)) {
                    err = "only 16-bit PCM mono/stereo is supported";
                    return false;
                }
                dataOffset_ = static_cast<uint64_t>(f_.tellg());
                dataBytes_ = size;
                pos_ = 0;
                return true;
            } else {
                f_.seekg(size + (size & 1), std::ios::cur);
            }
        }
        err = "no audio data";
        return false;
    }

    int read(std::vector<std::complex<double>>& out, int maxPairs)
    {
        if (!f_ || pos_ >= dataBytes_) return 1;
        const int bytesPerFrame = 2 * channels_;
        uint64_t remainingFrames = (dataBytes_ - pos_) / bytesPerFrame;
        int n = static_cast<int>(std::min<uint64_t>(remainingFrames, static_cast<uint64_t>(maxPairs)));
        if (n <= 0) return 1;

        std::vector<uint8_t> buf(static_cast<size_t>(n) * bytesPerFrame);
        f_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const size_t gotBytes = static_cast<size_t>(f_.gcount());
        const int gotFrames = static_cast<int>(gotBytes / bytesPerFrame);
        if (gotFrames <= 0) return 1;

        out.resize(static_cast<size_t>(gotFrames));
        for (int i = 0; i < gotFrames; ++i) {
            const uint8_t* p = buf.data() + static_cast<size_t>(i) * bytesPerFrame;
            const double I = static_cast<int16_t>(rd16(p)) / 32768.0;
            const double Q = channels_ == 2
                                 ? static_cast<int16_t>(rd16(p + 2)) / 32768.0
                                 : 0.0;
            out[static_cast<size_t>(i)] = std::complex<double>(I, Q);
        }
        pos_ += static_cast<uint64_t>(gotBytes);
        if (gotFrames < n) return 1;
        return 0;
    }

    double sampleRate() const { return static_cast<double>(rate_); }
    double progress() const
    {
        return dataBytes_ ? static_cast<double>(pos_) / static_cast<double>(dataBytes_) : -1.0;
    }

private:
    static uint16_t rd16(const uint8_t* p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    static uint32_t rd32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    std::ifstream f_;
    uint64_t dataOffset_ = 0;
    uint64_t dataBytes_ = 0;
    uint64_t pos_ = 0;
    int channels_ = 0;
    int audioFormat_ = 0;
    int bits_ = 0;
    uint32_t rate_ = 0;
};

} // namespace

Receiver::~Receiver() { stop(); }

void Receiver::setKeys(const std::vector<ChannelKey>& keys)
{
    std::lock_guard<std::mutex> lk(keyMu_);
    keys_ = keys;
    for (auto& k : keys_)
        if (k.builtin) k.enabled = true;
}

ReceiverStatus Receiver::status() const
{
    std::lock_guard<std::mutex> lk(statusMu_);
    return status_;
}

void Receiver::log(const std::string& line)
{
    if (logCb_) logCb_(line);
}

void Receiver::emit(const PacketRecord& p)
{
    if (packetCb_) packetCb_(p);
}

bool Receiver::start(const ReceiverConfig& cfg, std::string& err)
{
    if (cfg.sf < 5 || cfg.sf > 12) { err = "SF must be 5..12"; return false; }
    if (cfg.bw <= 0.0) { err = "bandwidth must be positive"; return false; }
    if (cfg.live) {
        if (cfg.rate == 0) { err = "sample rate must be positive"; return false; }
        if (std::fabs(cfg.rate / cfg.bw - std::llround(cfg.rate / cfg.bw)) > 1e-6) {
            err = "sample rate must be an integer multiple of bandwidth";
            return false;
        }
    } else if (cfg.wavPath.empty()) {
        err = "no WAV file selected";
        return false;
    }
    stop();
    cfg_ = cfg;
    running_.store(true);
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_ = ReceiverStatus{};
        status_.running = true;
        status_.source = cfg.live ? "RTL-SDR" : "IQ WAV";
        status_.freqHz = cfg.freqHz;
        status_.rate = cfg.rate;
        status_.progress = -1.0;
    }
    th_ = std::thread([this] { run(); });
    return true;
}

void Receiver::stop()
{
    running_.store(false);
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(statusMu_);
    status_.running = false;
}

void Receiver::run()
{
    if (cfg_.live)
        runLive();
    else
        runWav();
    running_.store(false);
    std::lock_guard<std::mutex> lk(statusMu_);
    status_.running = false;
}

void Receiver::runLive()
{
    mc::RtlSdr sdr;
    std::string err;
    if (!sdr.open(cfg_.device, err)) {
        log("RTL-SDR: " + err);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.lastError = err;
        return;
    }
    if (!sdr.configure(cfg_.freqHz, cfg_.rate, cfg_.gain, cfg_.ppm, cfg_.biasTee, err)) {
        log("RTL-SDR: " + err);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.lastError = err;
        return;
    }

    const uint32_t actualRate = sdr.actualSampleRate() ? sdr.actualSampleRate() : cfg_.rate;
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.device = sdr.deviceName();
        status_.freqHz = sdr.actualCenterFreq() ? sdr.actualCenterFreq() : cfg_.freqHz;
        status_.rate = actualRate;
    }

    mc::LoraParams params;
    params.sf = cfg_.sf;
    params.bw = cfg_.bw;
    params.fs = static_cast<double>(cfg_.rate);
    if (!mc::initLora(params, err)) {
        log("LoRa: " + err);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.lastError = err;
        return;
    }

    std::vector<uint8_t> raw(262144);
    auto source = [&](std::vector<std::complex<double>>& chunk, int maxPairs,
                      std::string& readErr) -> int {
        const int wantPairs = std::min(131072, maxPairs);
        int nRead = 0;
        if (sdr.read(raw.data(), wantPairs * 2, nRead, readErr) != 0) return -1;
        const int pairs = nRead / 2;
        chunk.resize(static_cast<size_t>(pairs));
        for (int i = 0; i < pairs; ++i) {
            const double I = (static_cast<int>(raw[static_cast<size_t>(2 * i)]) - 127.5) / 128.0;
            const double Q =
                (static_cast<int>(raw[static_cast<size_t>(2 * i + 1)]) - 127.5) / 128.0;
            chunk[static_cast<size_t>(i)] = std::complex<double>(I, Q);
        }
        return running_.load() ? 0 : 1;
    };

    runStream(params, source);
}

void Receiver::runWav()
{
    WavStream wav;
    std::string err;
    if (!wav.open(cfg_.wavPath, err)) {
        log("WAV: " + err);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.lastError = err;
        return;
    }
    mc::LoraParams params;
    params.sf = cfg_.sf;
    params.bw = cfg_.bw;
    params.fs = wav.sampleRate();
    if (!mc::initLora(params, err)) {
        log("LoRa: " + err);
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.lastError = err;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.rate = static_cast<uint32_t>(std::llround(wav.sampleRate()));
        status_.progress = 0.0;
    }

    auto source = [&](std::vector<std::complex<double>>& chunk, int maxPairs,
                      std::string&) -> int {
        if (!running_.load()) return 1;
        const int rc = wav.read(chunk, maxPairs);
        {
            std::lock_guard<std::mutex> lk(statusMu_);
            status_.progress = wav.progress();
        }
        return rc;
    };

    runStream(params, source);
}

bool Receiver::runStream(const mc::LoraParams& params,
                         const std::function<int(std::vector<std::complex<double>>&, int,
                                                 std::string&)>& source)
{
    const double rate = params.fs;
    const int blockSize = std::max(64, static_cast<int>(rate / 1000));
    const int chunkPairs = 131072;
    std::vector<std::complex<double>> chunk;
    chunk.reserve(static_cast<size_t>(chunkPairs));

    std::deque<std::complex<double>> preRoll;
    const size_t preRollMax = static_cast<size_t>(rate / 5);
    std::vector<std::complex<double>> capture;
    capture.reserve(static_cast<size_t>(rate) * 2);

    bool active = false;
    size_t quietSamples = 0;
    const size_t quietLimit = static_cast<size_t>(rate * 3 / 20);
    const size_t maxCapture = static_cast<size_t>(rate) * 2;

    double noiseFloor = 0.0;
    const double triggerFactor = std::pow(10.0, cfg_.thresholdDb / 20.0);

    const size_t noiseWindow = 1000;
    const size_t noiseWarmup = 50;
    const size_t quietWindow = 50;
    std::deque<double> noiseHistory;
    std::deque<double> activeHistory;
    std::vector<double> sortBuf;
    auto medianLevel = [&]() -> double {
        if (noiseHistory.empty()) return 1e-9;
        sortBuf.assign(noiseHistory.begin(), noiseHistory.end());
        const size_t mid = sortBuf.size() / 2;
        std::nth_element(sortBuf.begin(), sortBuf.begin() + mid, sortBuf.end());
        return std::max(sortBuf[mid], 1e-9);
    };

    BandDetector detector;
    detector.init(params.fs, params.bw);
    std::vector<double> blockLevels;
    long long globalSamples = 0;
    bool decodedOnce = false;
    unsigned long long triggers = 0;
    unsigned long long captures = 0;
    unsigned long long packets = 0;

    while (running_.load()) {
        std::string err;
        const int rc = source(chunk, chunkPairs, err);
        if (rc == 1) break;
        if (rc < 0) {
            if (!err.empty()) log(err);
            std::lock_guard<std::mutex> lk(statusMu_);
            status_.lastError = err;
            break;
        }
        const int pairs = static_cast<int>(chunk.size());
        if (pairs <= 0) continue;

        blockLevels.clear();
        blockLevels.reserve(static_cast<size_t>((pairs + blockSize - 1) / blockSize));
        for (int start = 0; start < pairs; start += blockSize) {
            const int n = std::min(blockSize, pairs - start);
            double sum = 0.0;
            for (int i = start; i < start + n; ++i) sum += detector.process(chunk[i]);
            blockLevels.push_back(std::sqrt(sum / static_cast<double>(n)));
        }

        if (!active) {
            int triggerBlock = -1;
            for (size_t bi = 0; bi < blockLevels.size(); ++bi) {
                const int start = static_cast<int>(bi) * blockSize;
                const double level = blockLevels[bi];
                noiseHistory.push_back(level);
                while (noiseHistory.size() > noiseWindow) noiseHistory.pop_front();
                noiseFloor = medianLevel();
                if (noiseHistory.size() >= noiseWarmup &&
                    level > noiseFloor * triggerFactor) {
                    triggerBlock = start;
                    break;
                }
            }
            if (triggerBlock >= 0) {
                active = true;
                quietSamples = 0;
                activeHistory.clear();
                capture.assign(preRoll.begin(), preRoll.end());
                const int endOfTrigger = std::min(pairs, triggerBlock + blockSize);
                capture.insert(capture.end(), chunk.begin(), chunk.begin() + endOfTrigger);
                capture.insert(capture.end(), chunk.begin() + endOfTrigger, chunk.end());
                preRoll.clear();
                ++triggers;
                if (cfg_.verbose) {
                    std::ostringstream os;
                    os << "trigger at "
                       << (static_cast<double>(globalSamples + triggerBlock) / rate) << " s"
                       << " (floor " << noiseFloor << ")";
                    log(os.str());
                }
            } else {
                for (const auto& x : chunk) preRoll.push_back(x);
                while (preRoll.size() > preRollMax) preRoll.pop_front();
            }
        } else {
            capture.insert(capture.end(), chunk.begin(), chunk.end());
            for (size_t bi = 0; bi < blockLevels.size(); ++bi) {
                const int start = static_cast<int>(bi) * blockSize;
                const int n = std::min(blockSize, pairs - start);
                const double level = blockLevels[bi];
                activeHistory.push_back(level);
                while (activeHistory.size() > quietWindow) activeHistory.pop_front();
                double activeMedian = level;
                if (activeHistory.size() >= quietWindow) {
                    sortBuf.assign(activeHistory.begin(), activeHistory.end());
                    const size_t mid = sortBuf.size() / 2;
                    std::nth_element(sortBuf.begin(), sortBuf.begin() + mid, sortBuf.end());
                    activeMedian = sortBuf[mid];
                }
                if (activeMedian < noiseFloor * 1.4)
                    quietSamples += static_cast<size_t>(n);
                else
                    quietSamples = 0;
            }

            if (quietSamples > quietLimit || capture.size() > maxCapture) {
                const double capSec = static_cast<double>(capture.size()) / rate;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = decodeCapture(capture, params);
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                ++captures;
                if (ok) ++packets;
                {
                    std::lock_guard<std::mutex> lk(statusMu_);
                    status_.noiseFloor = noiseFloor;
                    status_.lastCaptureSec = capSec;
                    status_.lastDecodeMs = ms;
                    status_.triggers = triggers;
                    status_.captures = captures;
                    status_.packets = packets;
                }
                if (cfg_.verbose) {
                    std::ostringstream os;
                    os << "capture " << capSec << " s -> " << (ok ? "decoded" : "no packet")
                       << " (" << ms << " ms)";
                    log(os.str());
                }
                if (ok) decodedOnce = true;
                active = false;
                quietSamples = 0;
                capture.clear();
                if (decodedOnce && !cfg_.decodeAll) break;
            }
        }
        globalSamples += pairs;
        {
            std::lock_guard<std::mutex> lk(statusMu_);
            status_.noiseFloor = noiseFloor;
            status_.triggers = triggers;
            status_.captures = captures;
            status_.packets = packets;
        }
    }

    // Flush a capture that was in progress when the source ended.
    if (running_.load() && active && !capture.empty()) {
        const double capSec = static_cast<double>(capture.size()) / rate;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = decodeCapture(capture, params);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        ++captures;
        if (ok) ++packets;
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.noiseFloor = noiseFloor;
        status_.lastCaptureSec = capSec;
        status_.lastDecodeMs = ms;
        status_.triggers = triggers;
        status_.captures = captures;
        status_.packets = packets;
    }
    return true;
}

bool Receiver::decodeCapture(const std::vector<std::complex<double>>& capture,
                             const mc::LoraParams& params)
{
    mc::LoraPacket pkt;
    std::string why;
    if (!mc::decodeLora(capture, params, pkt, why, false, 8)) return false;

    const int n = pkt.header.payloadLen;
    if (n <= 0 || static_cast<size_t>(n) > pkt.data.size()) return false;
    std::vector<uint8_t> mesh(pkt.data.begin(), pkt.data.begin() + n);

    mc::MeshCorePacket mp;
    std::string err;
    if (!mc::parseMeshCore(mesh, mp, err) || !mc::isMeshCoreV1(mp)) return false;

    PacketRecord rec;
    rec.tUnix = nowUnix();
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        rec.freqHz = status_.freqHz;
    }
    rec.header = mp.header;
    rec.version = mp.version;
    rec.route = mp.route;
    rec.type = mp.type;
    rec.hops = mp.hashCount;
    rec.path = mp.path;
    rec.payload = mesh;
    rec.rawHex = hexBytes(mesh);
    rec.quality = pkt.quality;
    rec.isMeshCore = true;
    rec.channelHash = 0;

    if (mp.type == 0x04) {
        mc::MeshCoreAdvert a;
        if (mc::parseAdvert(mp.payload, a)) {
            rec.isAdvert = true;
            rec.advert.pubkeyHex = hexBytes(a.publicKey, 32);
            std::memcpy(rec.advert.pubkey.data(), a.publicKey, 32);
            rec.advert.name = a.name;
            rec.advert.nodeType = a.nodeType;
            rec.advert.hasLocation = a.hasLocation;
            rec.advert.lat = a.hasLocation ? a.latitude / 1e6 : 0.0;
            rec.advert.lon = a.hasLocation ? a.longitude / 1e6 : 0.0;
            rec.advert.timestamp = a.timestamp;
            rec.advert.signatureValid = a.signatureValid;
            rec.advert.firstSeen = rec.tUnix;
            rec.advert.lastSeen = rec.tUnix;
            rec.advert.count = 1;
        }
    } else if (mp.type == 0x05 || mp.type == 0x06) {
        std::vector<ChannelKey> keys;
        {
            std::lock_guard<std::mutex> lk(keyMu_);
            keys = keys_;
        }
        bool matched = false;
        for (size_t ki = 0; ki < keys.size(); ++ki) {
            if (!keys[ki].enabled) continue;
            std::vector<uint8_t> plain;
            bool macOk = false;
            uint8_t expected = 0;
            if (!mc::decryptGroupPayload(mp, keys[ki].key, plain, macOk, expected))
                continue;
            rec.channelHash = expected;
            rec.channelLabel = keys[ki].label;
            rec.macOk = macOk;
            rec.keyIndex = static_cast<int>(ki);
            if (mp.type == 0x05) {
                uint32_t ts = 0;
                uint8_t flags = 0;
                std::string text;
                if (mc::parseGroupText(plain, ts, flags, text)) {
                    rec.isText = true;
                    rec.msgTimestamp = ts;
                    rec.msgFlags = flags;
                    rec.text = text;
                    const size_t sep = text.find(": ");
                    if (sep != std::string::npos) {
                        rec.sender = text.substr(0, sep);
                        rec.text = text.substr(sep + 2);
                    }
                }
            } else {
                uint16_t dt = 0;
                std::vector<uint8_t> data;
                if (mc::parseGroupData(plain, dt, data)) {
                    rec.isData = true;
                    rec.dataType = dt;
                    rec.data = std::move(data);
                }
            }
            matched = true;
            break;
        }
        if (!matched) {
            rec.channelHash = mp.payload.empty() ? 0 : mp.payload[0];
            rec.channelLabel = "Unknown";
        }
    }

    // Deduplicate identical payloads from the same path within 2 s.  The path
    // is included so direct and relayed copies remain visible.
    std::vector<uint8_t> hashBytes = mp.payload;
    hashBytes.insert(hashBytes.end(), mp.path.begin(), mp.path.end());
    hashBytes.push_back(mp.pathLenByte);
    hashBytes.push_back(mp.route);
    const uint64_t h = fnv1a(hashBytes.data(), hashBytes.size());
    while (!recent_.empty() && rec.tUnix - recent_.front().second > 2.0)
        recent_.erase(recent_.begin());
    for (const auto& r : recent_)
        if (r.first == h) return false;
    recent_.push_back({h, rec.tUnix});

    rec.seq = ++seq_;
    emit(rec);
    return true;
}

std::string defaultDataDir()
{
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path p =
        la ? std::filesystem::path(la) / "MeshCoreSniffer" : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

std::string defaultDbPath() { return (std::filesystem::path(defaultDataDir()) / "meshcore.db").string(); }

std::string defaultTileDir()
{
    std::filesystem::path p = std::filesystem::path(defaultDataDir()) / "tiles";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

} // namespace mcs::gui
