// receiver.h - GUI receiver thread (RTL-SDR or streaming IQ WAV).
#pragma once

#include <atomic>
#include <complex>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model.h"

namespace mc {
struct LoraParams;
}

namespace mcs::gui {

class Receiver {
public:
    using PacketCallback = std::function<void(const PacketRecord&)>;
    using TextCallback = std::function<void(const std::string&)>;

    Receiver() = default;
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void setPacketCallback(PacketCallback cb) { packetCb_ = std::move(cb); }
    void setLogCallback(TextCallback cb) { logCb_ = std::move(cb); }
    void setKeys(const std::vector<ChannelKey>& keys);

    bool start(const ReceiverConfig& cfg, std::string& err);
    void stop();
    bool running() const { return running_.load(); }
    ReceiverStatus status() const;

private:
    void run();
    void runLive();
    void runWav();
    bool runStream(const mc::LoraParams& params,
                   const std::function<int(std::vector<std::complex<double>>&, int,
                                           std::string&)>& source);
    bool decodeCapture(const std::vector<std::complex<double>>& capture,
                       const mc::LoraParams& params);
    void log(const std::string& line);
    void emit(const PacketRecord& p);

    ReceiverConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread th_;

    PacketCallback packetCb_;
    TextCallback logCb_;

    mutable std::mutex keyMu_;
    std::vector<ChannelKey> keys_;

    mutable std::mutex statusMu_;
    ReceiverStatus status_;

    // Duplicate suppression (worker thread only).
    std::vector<std::pair<uint64_t, double>> recent_;
    uint64_t seq_ = 0;
};

std::string defaultDataDir();
std::string defaultDbPath();
std::string defaultTileDir();

} // namespace mcs::gui
