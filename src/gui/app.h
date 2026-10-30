// app.h - GUI application state, thread-safe stores and shared helpers.
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "model.h"
#include "receiver.h"
#include "store.h"
#include "tiles.h"

namespace mcs::gui {

class PacketStore {
public:
    void add(const PacketRecord& p);
    std::vector<PacketRecord> snapshot(size_t maxItems) const;
    void clear();
    long total() const;
    std::map<int, long> typeCounts() const;
    std::map<uint8_t, long> channelCounts() const;

private:
    static constexpr size_t kCap = 50000;
    mutable std::mutex mu_;
    std::deque<PacketRecord> items_;
    long total_ = 0;
    std::map<int, long> types_;
    std::map<uint8_t, long> channels_;
};

class NodeStore {
public:
    void upsert(const NodeRecord& n);
    void replace(const std::vector<NodeRecord>& nodes);
    std::vector<NodeRecord> snapshot() const;
    bool get(const std::string& pubkeyHex, NodeRecord& out) const;
    void clear();
    size_t size() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, NodeRecord> nodes_;
};

class LogStore {
public:
    void add(const std::string& line);
    void addPrefixed(const std::string& prefix, const std::string& line);
    std::vector<std::string> snapshot(size_t maxItems) const;
    void clear();

private:
    static constexpr size_t kCap = 4000;
    mutable std::mutex mu_;
    std::deque<std::string> lines_;
};

struct AppState {
    Receiver receiver;
    Store store;
    PacketStore packets;
    NodeStore nodes;
    LogStore logs;
    TileCache tiles;
    MapView map;

    std::vector<ChannelKey> keys;
    mutable std::mutex keysMu;

    // Receiver configuration (UI model).
    bool cfgLive = true;
    int cfgDevice = 0;
    double cfgFreqMhz = 910.525;
    uint32_t cfgRate = 250000;
    std::string cfgGain = "auto";
    double cfgGainDb = 32.0;
    int cfgPpm = 0;
    bool cfgBiasTee = false;
    double cfgThreshold = 3.0;
    int cfgSf = 7;
    double cfgBwKhz = 62.5;
    bool cfgDecodeAll = true;
    bool cfgVerbose = false;
    bool cfgRecord = true;
    std::atomic<bool> recordToDb{true};
    char cfgWavPath[512] = "";

    std::vector<std::string> devices;
    std::string dataDir;
    std::string dbPath;

    ReceiverStatus status;
    std::string statusMsg;
    std::string lastError;

    // UI state.
    bool paused = false;
    bool autoScroll = true;
    bool mapAutoFit = true;
    bool mapOnline = true;
    bool showAbout = false;
    bool showClearConfirm = false;
    bool requestQuit = false;
    bool inspectRequested = false;
    std::vector<PacketRecord> frozen;
    std::string selectedNode;
    int selectedChannel = -1;  // channel hash, -1 = all
    uint64_t selectedSeq = 0;
    bool hasSelection = false;
    PacketRecord selectedPacket;
    Filters filters;

    // Keys editor.
    char keyLabel[64] = "";
    char keyText[512] = "";
    std::string keyError;

    // Frequency editor.
    char freqBuf[32] = "910.525";

    // Chat composer is intentionally absent: receiver is RX-only.

    void init();
    void shutdown();
    void refreshDevices();
    void loadSettings();
    void saveSettings();
    void buildConfig(ReceiverConfig& out) const;
    bool startReceiver();
    void stopReceiver();
    void restartReceiver();
    void applyFrequencyFromBuf();
    void selectBand(double mhz);
    void resetKeysToDefaults();
    std::vector<ChannelKey> keySnapshot() const;
    void pushKeysToReceiver();
    bool addKeyFromEditor();
    void removeKey(size_t index);
    void setKeyEnabled(size_t index, bool enabled);
    void observePacket(const PacketRecord& p);
    std::vector<PacketRecord> visiblePackets(size_t maxItems) const;
    bool matchFilter(const PacketRecord& p) const;
    std::string channelLabelFor(uint8_t hash) const;
    void addLog(const std::string& line);
    void clearHistory();
    void exportCsv(const std::string& path, const std::vector<PacketRecord>& view);
    void exportJson(const std::string& path, const std::vector<PacketRecord>& view);
};

// Shared helpers.
std::string hexs(const std::vector<uint8_t>& v, size_t maxn = SIZE_MAX);
std::string hexs(const uint8_t* d, size_t n);
std::string fmtLocalTime(double tUnix);
std::string fmtUtc(uint32_t t);
std::string fmtShortTime(double tUnix);
std::string typeName(int t);
std::string routeName(int r);
std::string nodeTypeName(int t);
std::string macStatus(const PacketRecord& p);
std::string packetSummary(const PacketRecord& p);
bool parseKeyText(const std::string& text, std::vector<uint8_t>& key);
uint8_t channelHash(const std::vector<uint8_t>& key);
std::string defaultDataDir();
std::string defaultDbPath();
std::string defaultTileDir();
std::string openFileDialog(const char* title, const char* filter, const char* defExt);
std::string saveFileDialog(const char* title, const char* filter, const char* defExt);

} // namespace mcs::gui
