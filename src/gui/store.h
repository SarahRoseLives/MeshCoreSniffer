// store.h - SQLite persistence for the GUI (packets, adverts, keys, settings).
#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "model.h"

struct sqlite3;

namespace mcs::gui {

class Store {
public:
    Store() = default;
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();
    bool isOpen() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }

    // Background write queue (safe from the receiver thread).
    void observe(const PacketRecord& p);

    long packetCount() const;
    std::vector<PacketRecord> recentPackets(int limit) const;
    std::vector<NodeRecord> loadNodes() const;
    std::vector<ChannelKey> loadKeys() const;

    bool saveKey(const ChannelKey& k, std::string& err);
    bool deleteKey(uint8_t hash, const std::string& label, std::string& err);
    bool setKeyEnabled(uint8_t hash, const std::string& label, bool enabled, std::string& err);

    std::string getSetting(const std::string& key, const std::string& def) const;
    bool setSetting(const std::string& key, const std::string& value, std::string& err);

    void clearHistory();

private:
    void writerLoop();
    void writeBatch(const std::vector<PacketRecord>& batch);
    bool initSchema(std::string& err);

    sqlite3* db_ = nullptr;
    std::string path_;

    std::thread th_;
    std::mutex qmu_;
    std::condition_variable cv_;
    std::queue<PacketRecord> q_;
    bool stop_ = false;

    mutable std::mutex db_mu_;
};

} // namespace mcs::gui
