#include "app.h"

#include "crypto.h"
#include "meshcore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace mcs::gui {
namespace {

const std::vector<uint8_t> kPublicKey = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

std::mutex g_timeMu;

double nowUnix()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}


std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
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
        if (c == ' ' || c == ':' || c == '-' || c == '\n' || c == '\r') continue;
        t.push_back(c);
    }
    if (t.empty() || (t.size() % 2) != 0) return false;
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
    if (t.empty() || t.size() % 4 == 1) return false;
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (int v : t) {
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string csvEscape(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o.push_back('"');
        o.push_back(c);
    }
    o.push_back('"');
    return o;
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o.push_back(c); break;
        }
    }
    return o;
}

#ifdef _WIN32
std::string toUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
#endif

} // namespace

// ---------------------------------------------------------------------------
// Stores
// ---------------------------------------------------------------------------
void PacketStore::add(const PacketRecord& p)
{
    std::lock_guard<std::mutex> lk(mu_);
    items_.push_back(p);
    if (items_.size() > kCap) items_.pop_front();
    ++total_;
    ++types_[p.type];
    if (p.type == 0x05 || p.type == 0x06) ++channels_[p.channelHash];
}

std::vector<PacketRecord> PacketStore::snapshot(size_t maxItems) const
{
    std::lock_guard<std::mutex> lk(mu_);
    const size_t n = std::min(maxItems, items_.size());
    return std::vector<PacketRecord>(items_.end() - static_cast<std::ptrdiff_t>(n), items_.end());
}

void PacketStore::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    items_.clear();
    total_ = 0;
    types_.clear();
    channels_.clear();
}

long PacketStore::total() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return total_;
}
std::map<int, long> PacketStore::typeCounts() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return types_;
}
std::map<uint8_t, long> PacketStore::channelCounts() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return channels_;
}

void NodeStore::upsert(const NodeRecord& n)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(n.pubkeyHex);
    if (it == nodes_.end()) {
        nodes_[n.pubkeyHex] = n;
    } else {
        NodeRecord& r = it->second;
        const double first = r.firstSeen;
        const int count = r.count;
        r = n;
        r.firstSeen = first;
        r.count = count + 1;
    }
}

void NodeStore::replace(const std::vector<NodeRecord>& nodes)
{
    std::lock_guard<std::mutex> lk(mu_);
    nodes_.clear();
    for (const auto& n : nodes) nodes_[n.pubkeyHex] = n;
}

std::vector<NodeRecord> NodeStore::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<NodeRecord> out;
    out.reserve(nodes_.size());
    for (const auto& kv : nodes_) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const NodeRecord& a, const NodeRecord& b) {
        return a.lastSeen > b.lastSeen;
    });
    return out;
}

bool NodeStore::get(const std::string& pubkeyHex, NodeRecord& out) const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(pubkeyHex);
    if (it == nodes_.end()) return false;
    out = it->second;
    return true;
}

void NodeStore::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    nodes_.clear();
}

size_t NodeStore::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return nodes_.size();
}

void LogStore::add(const std::string& line)
{
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back(line);
    if (lines_.size() > kCap) lines_.pop_front();
}

void LogStore::addPrefixed(const std::string& prefix, const std::string& line)
{
    add("[" + prefix + "] " + line);
}

std::vector<std::string> LogStore::snapshot(size_t maxItems) const
{
    std::lock_guard<std::mutex> lk(mu_);
    const size_t n = std::min(maxItems, lines_.size());
    return std::vector<std::string>(lines_.end() - static_cast<std::ptrdiff_t>(n), lines_.end());
}

void LogStore::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
}

// ---------------------------------------------------------------------------
// AppState
// ---------------------------------------------------------------------------
void AppState::init()
{
    dataDir = defaultDataDir();
    dbPath = defaultDbPath();
    std::string err;
    if (!store.open(dbPath, err)) {
        lastError = err;
        statusMsg = err;
    }
    addLog("MeshCoreSniffer GUI starting");
    addLog("Data: " + dataDir);

    if (store.isOpen()) {
        keys = store.loadKeys();
        bool havePublic = false;
        for (auto& k : keys)
            if (k.builtin && k.hash == channelHash(kPublicKey)) havePublic = true;
        if (!havePublic) {
            ChannelKey pub;
            pub.label = "Public";
            pub.key = kPublicKey;
            pub.hash = channelHash(kPublicKey);
            pub.builtin = true;
            pub.enabled = true;
            std::string e;
            if (store.saveKey(pub, e)) keys.push_back(pub);
            else addLog("key save: " + e);
        }
        for (auto& k : keys)
            if (k.builtin) k.enabled = true;

        nodes.replace(store.loadNodes());
        auto recent = store.recentPackets(2000);
        uint64_t seq = 1ULL << 40;
        for (auto& p : recent) {
            p.seq = seq++;
            packets.add(p);
        }
        loadSettings();
    }

    receiver.setPacketCallback([this](const PacketRecord& p) { observePacket(p); });
    receiver.setLogCallback([this](const std::string& s) { addLog(s); });
    receiver.setKeys(keySnapshot());
    status = receiver.status();
    tiles.init(defaultTileDir(), "");
    tiles.set_online(mapOnline);
    refreshDevices();
    recordToDb.store(cfgRecord);
    addLog("Ready");
}

void AppState::shutdown()
{
    receiver.stop();
    saveSettings();
    store.close();
}

void AppState::refreshDevices()
{
    devices.clear();
#ifdef _WIN32
    const char* candidates[] = {
        "librtlsdr.dll",
        "C:\\msys64\\mingw64\\bin\\librtlsdr.dll",
        "rtlsdr.dll",
    };
    HMODULE h = nullptr;
    for (const char* path : candidates) {
        if (std::strchr(path, '\\') || std::strchr(path, '/'))
            h = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        else
            h = LoadLibraryA(path);
        if (h) break;
    }
    if (h) {
        using count_fn = uint32_t (*)();
        using name_fn = const char* (*)(uint32_t);
        auto count = reinterpret_cast<count_fn>(
            reinterpret_cast<void*>(GetProcAddress(h, "rtlsdr_get_device_count")));
        auto name = reinterpret_cast<name_fn>(
            reinterpret_cast<void*>(GetProcAddress(h, "rtlsdr_get_device_name")));
        if (count) {
            const uint32_t n = count();
            for (uint32_t i = 0; i < n; ++i) {
                const char* nm = name ? name(i) : nullptr;
                devices.push_back(nm ? nm : ("RTL-SDR " + std::to_string(i)));
            }
        }
        FreeLibrary(h);
    }
#endif
    if (cfgDevice < 0 || cfgDevice >= static_cast<int>(devices.size())) cfgDevice = 0;
    if (devices.empty()) addLog("No RTL-SDR devices detected");
    else {
        std::ostringstream os;
        os << "Found " << devices.size() << " RTL-SDR device(s)";
        addLog(os.str());
    }
}

void AppState::loadSettings()
{
    if (!store.isOpen()) return;
    auto get = [&](const char* k, const std::string& d) { return store.getSetting(k, d); };
    cfgLive = get("source", "rtl") != "wav";
    cfgDevice = std::atoi(get("device", "0").c_str());
    cfgFreqMhz = std::atof(get("freq_mhz", "910.525").c_str());
    cfgRate = static_cast<uint32_t>(std::strtoul(get("rate", "250000").c_str(), nullptr, 10));
    cfgGain = get("gain", "auto");
    cfgGainDb = std::atof(get("gain_db", "32").c_str());
    cfgPpm = std::atoi(get("ppm", "0").c_str());
    cfgBiasTee = get("bias", "0") == "1";
    cfgThreshold = std::atof(get("threshold", "3").c_str());
    cfgSf = std::atoi(get("sf", "7").c_str());
    cfgBwKhz = std::atof(get("bw_khz", "62.5").c_str());
    cfgDecodeAll = get("decode_all", "1") == "1";
    cfgVerbose = get("verbose", "0") == "1";
    cfgRecord = get("record", "1") == "1";
    mapOnline = get("map_online", "1") == "1";
    const std::string wav = get("wav_path", "");
    std::snprintf(cfgWavPath, sizeof(cfgWavPath), "%s", wav.c_str());
    if (cfgRate != 250000 && cfgRate != 500000 && cfgRate != 1000000) cfgRate = 250000;
    if (cfgSf < 5 || cfgSf > 12) cfgSf = 7;
    if (cfgBwKhz <= 0.0) cfgBwKhz = 62.5;
    if (cfgThreshold < 0.0) cfgThreshold = 3.0;
    std::snprintf(freqBuf, sizeof(freqBuf), "%.6f", cfgFreqMhz);
    recordToDb.store(cfgRecord);
    addLog("Settings loaded");
}

void AppState::saveSettings()
{
    if (!store.isOpen()) return;
    auto set = [&](const char* k, const std::string& v) {
        std::string e;
        if (!store.setSetting(k, v, e)) addLog("setting: " + e);
    };
    set("source", cfgLive ? "rtl" : "wav");
    set("device", std::to_string(cfgDevice));
    set("freq_mhz", std::to_string(cfgFreqMhz));
    set("rate", std::to_string(cfgRate));
    set("gain", cfgGain);
    set("gain_db", std::to_string(cfgGainDb));
    set("ppm", std::to_string(cfgPpm));
    set("bias", cfgBiasTee ? "1" : "0");
    set("threshold", std::to_string(cfgThreshold));
    set("sf", std::to_string(cfgSf));
    set("bw_khz", std::to_string(cfgBwKhz));
    set("decode_all", cfgDecodeAll ? "1" : "0");
    set("verbose", cfgVerbose ? "1" : "0");
    set("record", cfgRecord ? "1" : "0");
    set("map_online", mapOnline ? "1" : "0");
    set("wav_path", cfgWavPath);
    recordToDb.store(cfgRecord);
}

void AppState::buildConfig(ReceiverConfig& out) const
{
    out = ReceiverConfig{};
    out.live = cfgLive;
    out.device = cfgDevice;
    out.freqHz = cfgFreqMhz * 1e6;
    out.rate = cfgRate;
    out.gain = (cfgGain == "auto") ? "auto" : std::to_string(cfgGainDb);
    out.ppm = cfgPpm;
    out.biasTee = cfgBiasTee;
    out.thresholdDb = cfgThreshold;
    out.sf = cfgSf;
    out.bw = cfgBwKhz * 1000.0;
    out.wavPath = cfgWavPath;
    out.decodeAll = cfgDecodeAll;
    out.verbose = cfgVerbose;
}

bool AppState::startReceiver()
{
    ReceiverConfig rc;
    buildConfig(rc);
    if (!rc.live && rc.wavPath.empty()) {
        statusMsg = "Select an IQ WAV file first";
        return false;
    }
    saveSettings();
    std::string err;
    if (!receiver.start(rc, err)) {
        statusMsg = err;
        lastError = err;
        addLog("start: " + err);
        return false;
    }
    statusMsg = rc.live ? "Listening..." : "Decoding WAV...";
    lastError.clear();
    addLog(statusMsg);
    return true;
}

void AppState::stopReceiver()
{
    receiver.stop();
    status = receiver.status();
    statusMsg = "Stopped";
    addLog("stopped");
    saveSettings();
}

void AppState::restartReceiver()
{
    const bool was = receiver.running();
    receiver.stop();
    if (was) startReceiver();
    else saveSettings();
}

void AppState::applyFrequencyFromBuf()
{
    try {
        const double v = std::stod(freqBuf);
        if (v > 0.0) {
            cfgFreqMhz = v;
            saveSettings();
        }
    } catch (...) {
    }
}

void AppState::selectBand(double mhz)
{
    cfgFreqMhz = mhz;
    std::snprintf(freqBuf, sizeof(freqBuf), "%.6f", cfgFreqMhz);
    saveSettings();
}

void AppState::resetKeysToDefaults()
{
    std::lock_guard<std::mutex> lk(keysMu);
    keys.clear();
    ChannelKey pub;
    pub.label = "Public";
    pub.key = kPublicKey;
    pub.hash = channelHash(kPublicKey);
    pub.builtin = true;
    pub.enabled = true;
    keys.push_back(pub);
    receiver.setKeys(keys);
}

std::vector<ChannelKey> AppState::keySnapshot() const
{
    std::lock_guard<std::mutex> lk(keysMu);
    return keys;
}

void AppState::pushKeysToReceiver()
{
    receiver.setKeys(keySnapshot());
}

bool AppState::addKeyFromEditor()
{
    std::vector<uint8_t> key;
    if (!parseKeyText(keyText, key)) {
        keyError = "expected 16 or 32 bytes as hex or base64";
        return false;
    }
    ChannelKey k;
    k.key = key;
    k.hash = channelHash(key);
    std::string label = trim(keyLabel);
    if (label.empty()) {
        char b[32];
        std::snprintf(b, sizeof(b), "Key 0x%02x", k.hash);
        label = b;
    }
    k.label = label;
    k.builtin = false;
    k.enabled = true;
    std::string err;
    if (!store.saveKey(k, err)) {
        keyError = err;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(keysMu);
        bool replaced = false;
        for (auto& e : keys) {
            if (e.hash == k.hash && e.label == k.label) {
                e = k;
                replaced = true;
                break;
            }
        }
        if (!replaced) keys.push_back(k);
        receiver.setKeys(keys);
    }
    keyLabel[0] = '\0';
    keyText[0] = '\0';
    keyError.clear();
    addLog("key added: " + label);
    return true;
}

void AppState::removeKey(size_t index)
{
    ChannelKey k;
    {
        std::lock_guard<std::mutex> lk(keysMu);
        if (index >= keys.size() || keys[index].builtin) return;
        k = keys[index];
        std::string err;
        if (!store.deleteKey(k.hash, k.label, err)) addLog("key delete: " + err);
        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
        receiver.setKeys(keys);
    }
    addLog("key removed: " + k.label);
}

void AppState::setKeyEnabled(size_t index, bool enabled)
{
    ChannelKey k;
    {
        std::lock_guard<std::mutex> lk(keysMu);
        if (index >= keys.size()) return;
        if (keys[index].builtin) enabled = true;
        keys[index].enabled = enabled;
        k = keys[index];
        std::string err;
        if (!store.setKeyEnabled(k.hash, k.label, enabled, err)) addLog("key update: " + err);
        receiver.setKeys(keys);
    }
}

void AppState::observePacket(const PacketRecord& p)
{
    packets.add(p);
    if (p.isAdvert) nodes.upsert(p.advert);
    if (recordToDb.load()) store.observe(p);
}

bool AppState::matchFilter(const PacketRecord& p) const
{
    if (!filters.showRelays && p.hops > 0) return false;
    if (filters.type >= 0 && p.type != filters.type) return false;
    if (filters.route >= 0 && p.route != filters.route) return false;
    if (filters.channel >= 0 && (int)p.channelHash != filters.channel) return false;
    if (filters.text[0]) {
        const std::string hay = lower(packetSummary(p) + " " + p.sender + " " + p.text + " " +
                                      p.channelLabel + " " + p.rawHex);
        if (hay.find(lower(filters.text)) == std::string::npos) return false;
    }
    return true;
}

std::vector<PacketRecord> AppState::visiblePackets(size_t maxItems) const
{
    auto all = packets.snapshot(maxItems);
    std::vector<PacketRecord> out;
    out.reserve(all.size());
    for (const auto& p : all)
        if (matchFilter(p)) out.push_back(p);
    return out;
}

std::string AppState::channelLabelFor(uint8_t hash) const
{
    std::lock_guard<std::mutex> lk(keysMu);
    for (const auto& k : keys)
        if (k.hash == hash) return k.label;
    char b[32];
    std::snprintf(b, sizeof(b), "Unknown 0x%02x", hash);
    return b;
}

void AppState::addLog(const std::string& line)
{
    logs.add("[" + fmtLocalTime(nowUnix()) + "] " + line);
}

void AppState::clearHistory()
{
    packets.clear();
    nodes.clear();
    store.clearHistory();
    {
        std::lock_guard<std::mutex> lk(keysMu);
        receiver.setKeys(keys);
    }
    hasSelection = false;
    selectedSeq = 0;
    selectedNode.clear();
    paused = false;
    frozen.clear();
    mapAutoFit = true;
    addLog("history cleared");
}

void AppState::exportCsv(const std::string& path, const std::vector<PacketRecord>& view)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        addLog("export failed: " + path);
        return;
    }
    f << "time,freq_hz,type,route,hops,channel,key,mac,sender,text,raw_hex\n";
    for (const auto& p : view) {
        f << csvEscape(fmtLocalTime(p.tUnix)) << ',' << p.freqHz << ','
          << csvEscape(typeName(p.type)) << ',' << csvEscape(routeName(p.route)) << ','
          << p.hops << ',' << static_cast<int>(p.channelHash) << ','
          << csvEscape(p.channelLabel) << ',' << csvEscape(macStatus(p)) << ','
          << csvEscape(p.sender) << ',' << csvEscape(p.text) << ','
          << csvEscape(p.rawHex) << '\n';
    }
    addLog("exported CSV: " + path);
}

void AppState::exportJson(const std::string& path, const std::vector<PacketRecord>& view)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        addLog("export failed: " + path);
        return;
    }
    f << "[\n";
    for (size_t i = 0; i < view.size(); ++i) {
        const auto& p = view[i];
        f << "  {\"time\":" << p.tUnix << ",\"freq_hz\":" << p.freqHz
          << ",\"type\":\"" << jsonEscape(typeName(p.type)) << "\""
          << ",\"route\":\"" << jsonEscape(routeName(p.route)) << "\""
          << ",\"hops\":" << p.hops << ",\"channel\":" << static_cast<int>(p.channelHash)
          << ",\"key\":\"" << jsonEscape(p.channelLabel) << "\""
          << ",\"mac\":\"" << jsonEscape(macStatus(p)) << "\""
          << ",\"sender\":\"" << jsonEscape(p.sender) << "\""
          << ",\"text\":\"" << jsonEscape(p.text) << "\""
          << ",\"raw\":\"" << jsonEscape(p.rawHex) << "\"}";
        if (i + 1 < view.size()) f << ',';
        f << '\n';
    }
    f << "]\n";
    addLog("exported JSON: " + path);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string hexs(const uint8_t* d, size_t n)
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

std::string hexs(const std::vector<uint8_t>& v, size_t maxn)
{
    const size_t n = std::min(maxn, v.size());
    std::string s = hexs(v.data(), n);
    if (n < v.size()) s += "...";
    return s;
}

std::string fmtLocalTime(double tUnix)
{
    std::lock_guard<std::mutex> lk(g_timeMu);
    std::time_t t = static_cast<std::time_t>(tUnix);
    std::tm* tmp = std::localtime(&t);
    if (!tmp) return "--:--:--";
    char b[32];
    std::strftime(b, sizeof(b), "%H:%M:%S", tmp);
    return b;
}

std::string fmtShortTime(double tUnix)
{
    std::lock_guard<std::mutex> lk(g_timeMu);
    std::time_t t = static_cast<std::time_t>(tUnix);
    std::tm* tmp = std::localtime(&t);
    if (!tmp) return "(invalid)";
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", tmp);
    return b;
}

std::string fmtUtc(uint32_t timestamp)
{
    std::lock_guard<std::mutex> lk(g_timeMu);
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm* tmp = std::gmtime(&t);
    if (!tmp) return "(invalid)";
    char b[48];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", tmp);
    return b;
}

std::string typeName(int t) { return mc::meshcorePayloadName(t); }
std::string routeName(int r) { return mc::meshcoreRouteName(r); }
std::string nodeTypeName(int t) { return mc::meshcoreNodeTypeName(t); }

std::string macStatus(const PacketRecord& p)
{
    if (p.type != 0x05 && p.type != 0x06) return "-";
    if (!p.channelLabel.empty() && p.channelLabel != "Unknown")
        return p.macOk ? "MAC ok" : "MAC bad";
    return "no key";
}

std::string packetSummary(const PacketRecord& p)
{
    std::ostringstream os;
    if (p.isAdvert) {
        os << "ADVERT " << nodeTypeName(p.advert.nodeType) << " " << p.advert.name
           << " sig=" << (p.advert.signatureValid ? "valid" : "INVALID");
    } else if (p.isText) {
        if (!p.sender.empty()) os << p.sender << ": ";
        os << p.text;
    } else if (p.isData) {
        os << "GRP_DATA type=0x" << std::hex << p.dataType << std::dec << " len="
           << p.data.size();
    } else {
        os << typeName(p.type) << " " << p.payload.size() << " bytes";
    }
    return os.str();
}

bool parseKeyText(const std::string& text, std::vector<uint8_t>& key)
{
    key.clear();
    if (fromHex(text, key) && (key.size() == 16 || key.size() == 32)) return true;
    key.clear();
    if (fromBase64(text, key) && (key.size() == 16 || key.size() == 32)) return true;
    key.clear();
    return false;
}

uint8_t channelHash(const std::vector<uint8_t>& key)
{
    const auto h = mc::sha256(key.data(), key.size());
    return h[0];
}

#ifdef _WIN32
std::string openFileDialog(const char* title, const char* filter, const char* defExt)
{
    wchar_t file[1024] = {0};
    std::wstring wfilter;
    for (const char* p = filter; *p || *(p + 1); ++p) {
        if (*p == '\0') {
            wfilter.push_back(L'\0');
            if (!*(p + 1)) break;
        } else {
            wfilter.push_back(static_cast<wchar_t>(*p));
        }
    }
    wfilter.push_back(L'\0');
    wfilter.push_back(L'\0');
    const std::wstring wtitle = title ? widen(title) : std::wstring();
    const std::wstring wdef = defExt ? widen(defExt) : std::wstring();
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    ofn.lpstrDefExt = wdef.empty() ? nullptr : wdef.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return toUtf8(file);
}

std::string saveFileDialog(const char* title, const char* filter, const char* defExt)
{
    wchar_t file[1024] = {0};
    std::wstring wfilter;
    for (const char* p = filter; *p || *(p + 1); ++p) {
        if (*p == '\0') {
            wfilter.push_back(L'\0');
            if (!*(p + 1)) break;
        } else {
            wfilter.push_back(static_cast<wchar_t>(*p));
        }
    }
    wfilter.push_back(L'\0');
    wfilter.push_back(L'\0');
    const std::wstring wtitle = title ? widen(title) : std::wstring();
    const std::wstring wdef = defExt ? widen(defExt) : std::wstring();
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    ofn.lpstrDefExt = wdef.empty() ? nullptr : wdef.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return toUtf8(file);
}
#else
std::string openFileDialog(const char*, const char*, const char*) { return {}; }
std::string saveFileDialog(const char*, const char*, const char*) { return {}; }
#endif

} // namespace mcs::gui
