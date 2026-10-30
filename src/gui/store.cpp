#include "store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <sqlite3.h>

namespace mcs::gui {
namespace {

void hexBytes(const uint8_t* d, size_t n, std::string& out)
{
    static const char* k = "0123456789abcdef";
    out.clear();
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(k[d[i] >> 4]);
        out.push_back(k[d[i] & 0xf]);
    }
}

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS packets("
    " id INTEGER PRIMARY KEY,"
    " t_unix REAL NOT NULL, freq_hz REAL NOT NULL,"
    " header INTEGER, version INTEGER, route INTEGER, type INTEGER,"
    " hops INTEGER, path BLOB,"
    " channel_hash INTEGER, channel_label TEXT, mac_ok INTEGER, key_index INTEGER,"
    " quality REAL, payload BLOB, raw_hex TEXT,"
    " is_advert INTEGER, adv_pubkey BLOB, adv_name TEXT, adv_node_type INTEGER,"
    " adv_has_loc INTEGER, adv_lat REAL, adv_lon REAL, adv_ts INTEGER, adv_sig INTEGER,"
    " is_text INTEGER, msg_ts INTEGER, msg_flags INTEGER, sender TEXT, text TEXT,"
    " is_data INTEGER, data_type INTEGER, data BLOB,"
    " is_meshcore INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_packets_t ON packets(t_unix);"
    "CREATE INDEX IF NOT EXISTS idx_packets_type ON packets(type);"
    "CREATE INDEX IF NOT EXISTS idx_packets_ch ON packets(channel_hash);"
    "CREATE TABLE IF NOT EXISTS adverts("
    " pubkey BLOB PRIMARY KEY, pubkey_hex TEXT NOT NULL,"
    " name TEXT, node_type INTEGER, has_loc INTEGER, lat REAL, lon REAL,"
    " ts INTEGER, sig INTEGER, first_seen REAL, last_seen REAL, count INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_adverts_seen ON adverts(last_seen);"
    "CREATE TABLE IF NOT EXISTS channels("
    " hash INTEGER NOT NULL, label TEXT NOT NULL, key BLOB NOT NULL,"
    " builtin INTEGER NOT NULL, enabled INTEGER NOT NULL,"
    " PRIMARY KEY(hash, label));"
    "CREATE TABLE IF NOT EXISTS settings("
    " key TEXT PRIMARY KEY, value TEXT NOT NULL);";

void bindBlob(sqlite3_stmt* st, int idx, const std::vector<uint8_t>& v)
{
    if (v.empty())
        sqlite3_bind_null(st, idx);
    else
        sqlite3_bind_blob(st, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

std::vector<uint8_t> columnBlob(sqlite3_stmt* st, int idx)
{
    const void* p = sqlite3_column_blob(st, idx);
    const int n = sqlite3_column_bytes(st, idx);
    if (!p || n <= 0) return {};
    return std::vector<uint8_t>(static_cast<const uint8_t*>(p),
                                static_cast<const uint8_t*>(p) + n);
}

std::string columnText(sqlite3_stmt* st, int idx)
{
    const unsigned char* s = sqlite3_column_text(st, idx);
    return s ? reinterpret_cast<const char*>(s) : "";
}

} // namespace

Store::~Store() { close(); }

bool Store::open(const std::string& path, std::string& err)
{
    if (db_) return true;
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        err = std::string("cannot open database: ") + (db ? sqlite3_errmsg(db) : "?");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    db_ = db;
    path_ = path;
    if (!initSchema(err)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    stop_ = false;
    th_ = std::thread([this] { writerLoop(); });
    return true;
}

bool Store::initSchema(std::string& err)
{
    char* e = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &e) != SQLITE_OK) {
        err = std::string("schema: ") + (e ? e : "?");
        sqlite3_free(e);
        return false;
    }
    return true;
}

void Store::close()
{
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(db_mu_);
    sqlite3_close(db_);
    db_ = nullptr;
}

void Store::observe(const PacketRecord& p)
{
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        q_.push(p);
    }
    cv_.notify_one();
}

void Store::writerLoop()
{
    for (;;) {
        std::vector<PacketRecord> batch;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            cv_.wait_for(lk, std::chrono::milliseconds(400),
                         [this] { return stop_ || !q_.empty(); });
            while (!q_.empty()) {
                batch.push_back(std::move(q_.front()));
                q_.pop();
            }
        }
        if (!batch.empty()) writeBatch(batch);
        if (stop_ && batch.empty()) break;
    }
}

void Store::writeBatch(const std::vector<PacketRecord>& batch)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return;
    sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* pkt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO packets("
        " t_unix,freq_hz,header,version,route,type,hops,path,"
        " channel_hash,channel_label,mac_ok,key_index,quality,payload,raw_hex,"
        " is_advert,adv_pubkey,adv_name,adv_node_type,adv_has_loc,adv_lat,adv_lon,"
        " adv_ts,adv_sig,is_text,msg_ts,msg_flags,sender,text,is_data,data_type,data,is_meshcore)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &pkt, nullptr);

    sqlite3_stmt* adv = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO adverts(pubkey,pubkey_hex,name,node_type,has_loc,lat,lon,ts,sig,"
        " first_seen,last_seen,count) VALUES(?,?,?,?,?,?,?,?,?,?,?,1)"
        " ON CONFLICT(pubkey) DO UPDATE SET"
        " pubkey_hex=excluded.pubkey_hex,name=excluded.name,node_type=excluded.node_type,"
        " has_loc=excluded.has_loc,lat=excluded.lat,lon=excluded.lon,ts=excluded.ts,"
        " sig=excluded.sig,last_seen=excluded.last_seen,count=adverts.count+1;",
        -1, &adv, nullptr);

    for (const PacketRecord& p : batch) {
        if (pkt) {
            int i = 1;
            sqlite3_bind_double(pkt, i++, p.tUnix);
            sqlite3_bind_double(pkt, i++, p.freqHz);
            sqlite3_bind_int(pkt, i++, p.header);
            sqlite3_bind_int(pkt, i++, p.version);
            sqlite3_bind_int(pkt, i++, p.route);
            sqlite3_bind_int(pkt, i++, p.type);
            sqlite3_bind_int(pkt, i++, p.hops);
            bindBlob(pkt, i++, p.path);
            sqlite3_bind_int(pkt, i++, p.channelHash);
            sqlite3_bind_text(pkt, i++, p.channelLabel.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(pkt, i++, p.macOk ? 1 : 0);
            sqlite3_bind_int(pkt, i++, p.keyIndex);
            sqlite3_bind_double(pkt, i++, p.quality);
            bindBlob(pkt, i++, p.payload);
            sqlite3_bind_text(pkt, i++, p.rawHex.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(pkt, i++, p.isAdvert ? 1 : 0);
            std::vector<uint8_t> pub(p.advert.pubkey.begin(), p.advert.pubkey.end());
            bindBlob(pkt, i++, pub);
            sqlite3_bind_text(pkt, i++, p.advert.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(pkt, i++, p.advert.nodeType);
            sqlite3_bind_int(pkt, i++, p.advert.hasLocation ? 1 : 0);
            sqlite3_bind_double(pkt, i++, p.advert.lat);
            sqlite3_bind_double(pkt, i++, p.advert.lon);
            sqlite3_bind_int64(pkt, i++, p.advert.timestamp);
            sqlite3_bind_int(pkt, i++, p.advert.signatureValid ? 1 : 0);
            sqlite3_bind_int(pkt, i++, p.isText ? 1 : 0);
            sqlite3_bind_int64(pkt, i++, p.msgTimestamp);
            sqlite3_bind_int(pkt, i++, p.msgFlags);
            sqlite3_bind_text(pkt, i++, p.sender.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(pkt, i++, p.text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(pkt, i++, p.isData ? 1 : 0);
            sqlite3_bind_int(pkt, i++, p.dataType);
            bindBlob(pkt, i++, p.data);
            sqlite3_bind_int(pkt, i++, p.isMeshCore ? 1 : 0);
            sqlite3_step(pkt);
            sqlite3_reset(pkt);
        }

        if (p.isAdvert && adv) {
            std::vector<uint8_t> pub(p.advert.pubkey.begin(), p.advert.pubkey.end());
            sqlite3_bind_blob(adv, 1, pub.data(), 32, SQLITE_TRANSIENT);
            sqlite3_bind_text(adv, 2, p.advert.pubkeyHex.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(adv, 3, p.advert.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(adv, 4, p.advert.nodeType);
            sqlite3_bind_int(adv, 5, p.advert.hasLocation ? 1 : 0);
            sqlite3_bind_double(adv, 6, p.advert.lat);
            sqlite3_bind_double(adv, 7, p.advert.lon);
            sqlite3_bind_int64(adv, 8, p.advert.timestamp);
            sqlite3_bind_int(adv, 9, p.advert.signatureValid ? 1 : 0);
            sqlite3_bind_double(adv, 10, p.tUnix);
            sqlite3_bind_double(adv, 11, p.tUnix);
            sqlite3_step(adv);
            sqlite3_reset(adv);
        }
    }

    if (pkt) sqlite3_finalize(pkt);
    if (adv) sqlite3_finalize(adv);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

long Store::packetCount() const
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    long n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM packets;", -1, &st, nullptr) ==
        SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = static_cast<long>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    return n;
}

std::vector<PacketRecord> Store::recentPackets(int limit) const
{
    std::lock_guard<std::mutex> lk(db_mu_);
    std::vector<PacketRecord> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT t_unix,freq_hz,header,version,route,type,hops,path,"
            "channel_hash,channel_label,mac_ok,key_index,quality,payload,raw_hex,"
            "is_advert,adv_pubkey,adv_name,adv_node_type,adv_has_loc,adv_lat,adv_lon,"
            "adv_ts,adv_sig,is_text,msg_ts,msg_flags,sender,text,is_data,data_type,data,"
            "is_meshcore FROM packets ORDER BY id DESC LIMIT ?;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            PacketRecord p;
            int c = 0;
            p.tUnix = sqlite3_column_double(st, c++);
            p.freqHz = sqlite3_column_double(st, c++);
            p.header = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.version = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.route = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.type = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.hops = sqlite3_column_int(st, c++);
            p.path = columnBlob(st, c++);
            p.channelHash = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.channelLabel = columnText(st, c++);
            p.macOk = sqlite3_column_int(st, c++) != 0;
            p.keyIndex = sqlite3_column_int(st, c++);
            p.quality = sqlite3_column_double(st, c++);
            p.payload = columnBlob(st, c++);
            p.rawHex = columnText(st, c++);
            p.isAdvert = sqlite3_column_int(st, c++) != 0;
            auto pub = columnBlob(st, c++);
            if (pub.size() == 32) {
                std::memcpy(p.advert.pubkey.data(), pub.data(), 32);
                hexBytes(p.advert.pubkey.data(), 32, p.advert.pubkeyHex);
            }
            p.advert.name = columnText(st, c++);
            p.advert.nodeType = sqlite3_column_int(st, c++);
            p.advert.hasLocation = sqlite3_column_int(st, c++) != 0;
            p.advert.lat = sqlite3_column_double(st, c++);
            p.advert.lon = sqlite3_column_double(st, c++);
            p.advert.timestamp = static_cast<uint32_t>(sqlite3_column_int64(st, c++));
            p.advert.signatureValid = sqlite3_column_int(st, c++) != 0;
            p.isText = sqlite3_column_int(st, c++) != 0;
            p.msgTimestamp = static_cast<uint32_t>(sqlite3_column_int64(st, c++));
            p.msgFlags = static_cast<uint8_t>(sqlite3_column_int(st, c++));
            p.sender = columnText(st, c++);
            p.text = columnText(st, c++);
            p.isData = sqlite3_column_int(st, c++) != 0;
            p.dataType = static_cast<uint16_t>(sqlite3_column_int(st, c++));
            p.data = columnBlob(st, c++);
            p.isMeshCore = sqlite3_column_int(st, c++) != 0;
            out.push_back(std::move(p));
        }
        sqlite3_finalize(st);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<NodeRecord> Store::loadNodes() const
{
    std::lock_guard<std::mutex> lk(db_mu_);
    std::vector<NodeRecord> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT pubkey,pubkey_hex,name,node_type,has_loc,lat,lon,ts,sig,"
            "first_seen,last_seen,count FROM adverts ORDER BY last_seen DESC;",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            NodeRecord n;
            auto pub = columnBlob(st, 0);
            if (pub.size() == 32) std::memcpy(n.pubkey.data(), pub.data(), 32);
            n.pubkeyHex = columnText(st, 1);
            n.name = columnText(st, 2);
            n.nodeType = sqlite3_column_int(st, 3);
            n.hasLocation = sqlite3_column_int(st, 4) != 0;
            n.lat = sqlite3_column_double(st, 5);
            n.lon = sqlite3_column_double(st, 6);
            n.timestamp = static_cast<uint32_t>(sqlite3_column_int64(st, 7));
            n.signatureValid = sqlite3_column_int(st, 8) != 0;
            n.firstSeen = sqlite3_column_double(st, 9);
            n.lastSeen = sqlite3_column_double(st, 10);
            n.count = sqlite3_column_int(st, 11);
            out.push_back(std::move(n));
        }
        sqlite3_finalize(st);
    }
    return out;
}

std::vector<ChannelKey> Store::loadKeys() const
{
    std::lock_guard<std::mutex> lk(db_mu_);
    std::vector<ChannelKey> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT hash,label,key,builtin,enabled FROM channels ORDER BY builtin DESC,label;",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            ChannelKey k;
            k.hash = static_cast<uint8_t>(sqlite3_column_int(st, 0));
            k.label = columnText(st, 1);
            k.key = columnBlob(st, 2);
            k.builtin = sqlite3_column_int(st, 3) != 0;
            k.enabled = sqlite3_column_int(st, 4) != 0;
            out.push_back(std::move(k));
        }
        sqlite3_finalize(st);
    }
    return out;
}

bool Store::saveKey(const ChannelKey& k, std::string& err)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) { err = "database not open"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO channels(hash,label,key,builtin,enabled)"
            " VALUES(?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(st, 1, k.hash);
    sqlite3_bind_text(st, 2, k.label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 3, k.key.data(), static_cast<int>(k.key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, k.builtin ? 1 : 0);
    sqlite3_bind_int(st, 5, k.enabled ? 1 : 0);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::deleteKey(uint8_t hash, const std::string& label, std::string& err)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) { err = "database not open"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM channels WHERE hash=? AND label=?;", -1, &st,
                           nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(st, 1, hash);
    sqlite3_bind_text(st, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::setKeyEnabled(uint8_t hash, const std::string& label, bool enabled, std::string& err)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) { err = "database not open"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE channels SET enabled=? WHERE hash=? AND label=?;", -1,
                           &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(st, 1, enabled ? 1 : 0);
    sqlite3_bind_int(st, 2, hash);
    sqlite3_bind_text(st, 3, label.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

std::string Store::getSetting(const std::string& key, const std::string& def) const
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return def;
    sqlite3_stmt* st = nullptr;
    std::string out = def;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM settings WHERE key=?;", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) out = columnText(st, 0);
        sqlite3_finalize(st);
    }
    return out;
}

bool Store::setSetting(const std::string& key, const std::string& value, std::string& err)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) { err = "database not open"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?);", -1, &st,
            nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

void Store::clearHistory()
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return;
    sqlite3_exec(db_, "DELETE FROM packets; DELETE FROM adverts;", nullptr, nullptr, nullptr);
}

} // namespace mcs::gui
