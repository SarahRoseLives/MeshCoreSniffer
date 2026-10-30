#include "panels.h"

#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace mcs::gui {
namespace {

using namespace theme;

struct BandPreset {
    const char* name;
    double mhz;
};

const BandPreset kBands[] = {
    {"US 910.525", 910.525},
    {"EU 869.525", 869.525},
};

const char* kAboutText =
    "MeshCoreSniffer GUI\n"
    "Receive-only MeshCore LoRa decoder for RTL-SDR and IQ WAV captures.\n\n"
    "The command-line MeshCoreSniffer.exe remains unchanged.\n"
    "History, keys and settings are stored under %LOCALAPPDATA%\\MeshCoreSniffer.\n\n"
    "Map tiles: Esri World Imagery by default; override with MESHCORE_TILE_URL.";

void draw_band_preset(AppState& s)
{
    ImGui::TextColored(dim, "band");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    int cur = -1;
    for (int i = 0; i < (int)(sizeof(kBands) / sizeof(kBands[0])); ++i)
        if (std::fabs(s.cfgFreqMhz - kBands[i].mhz) < 1e-4) cur = i;
    const char* label = cur >= 0 ? kBands[cur].name : "Custom";
    if (ImGui::BeginCombo("##band", label)) {
        for (int i = 0; i < (int)(sizeof(kBands) / sizeof(kBands[0])); ++i) {
            if (ImGui::Selectable(kBands[i].name, i == cur)) s.selectBand(kBands[i].mhz);
        }
        ImGui::EndCombo();
    }
}

void draw_freq_editor(AppState& s)
{
    ImGui::TextColored(dim, "freq MHz");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(105);
    if (ImGui::InputText("##freq", s.freqBuf, sizeof(s.freqBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.applyFrequencyFromBuf();
    }
    ImGui::SameLine(0, 4);
    if (ImGui::SmallButton("set")) s.applyFrequencyFromBuf();
}

void draw_gain(AppState& s)
{
    ImGui::TextColored(dim, "gain");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92);
    const bool autoGain = s.cfgGain == "auto";
    if (ImGui::BeginCombo("##gainmode", autoGain ? "auto" : "manual")) {
        if (ImGui::Selectable("auto", autoGain)) s.cfgGain = "auto";
        if (ImGui::Selectable("manual", !autoGain)) s.cfgGain = "manual";
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(autoGain);
    ImGui::SetNextItemWidth(130);
    float gainDb = (float)s.cfgGainDb;
    if (ImGui::SliderFloat("##gaindb", &gainDb, 0.0f, 49.6f, "%.1f dB",
                           ImGuiSliderFlags_AlwaysClamp))
        s.cfgGainDb = gainDb;
    ImGui::EndDisabled();
}

void draw_rate(AppState& s)
{
    ImGui::TextColored(dim, "rate");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115);
    const uint32_t rates[] = {250000, 500000, 1000000};
    int cur = 0;
    for (int i = 0; i < 3; ++i)
        if (rates[i] == s.cfgRate) cur = i;
    char curLabel[32];
    std::snprintf(curLabel, sizeof(curLabel), "%u Hz", s.cfgRate);
    if (ImGui::BeginCombo("##rate", curLabel)) {
        char b[32];
        for (int i = 0; i < 3; ++i) {
            std::snprintf(b, sizeof(b), "%u Hz", rates[i]);
            if (ImGui::Selectable(b, i == cur)) s.cfgRate = rates[i];
        }
        ImGui::EndCombo();
    }
}

void draw_sf_bw(AppState& s)
{
    ImGui::TextColored(dim, "SF");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(62);
    const std::string sfLabel = "SF" + std::to_string(s.cfgSf);
    if (ImGui::BeginCombo("##sf", sfLabel.c_str())) {
        for (int v = 5; v <= 12; ++v) {
            const std::string b = "SF" + std::to_string(v);
            if (ImGui::Selectable(b.c_str(), v == s.cfgSf)) s.cfgSf = v;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 8);
    ImGui::TextColored(dim, "BW");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92);
    const double bws[] = {62.5, 125.0, 250.0, 500.0};
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f kHz", s.cfgBwKhz);
    if (ImGui::BeginCombo("##bw", b)) {
        char label[32];
        for (double v : bws) {
            std::snprintf(label, sizeof(label), "%.1f kHz", v);
            if (ImGui::Selectable(label, std::fabs(v - s.cfgBwKhz) < 1e-6)) s.cfgBwKhz = v;
        }
        ImGui::EndCombo();
    }
}

void draw_source_row(AppState& s)
{
    if (s.receiver.running()) {
        if (ImGui::Button("Stop", ImVec2(64, 0))) s.stopReceiver();
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Restart", ImVec2(74, 0))) s.restartReceiver();
    } else {
        if (ImGui::Button("Start", ImVec2(64, 0))) s.startReceiver();
        ImGui::SameLine(0, 4);
        ImGui::BeginDisabled(true);
        ImGui::Button("Restart", ImVec2(74, 0));
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 10);
    ImGui::TextColored(dim, "source");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(118);
    if (ImGui::BeginCombo("##source", s.cfgLive ? "RTL-SDR" : "IQ WAV")) {
        if (ImGui::Selectable("RTL-SDR", s.cfgLive)) s.cfgLive = true;
        if (ImGui::Selectable("IQ WAV", !s.cfgLive)) s.cfgLive = false;
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 8);
    ImGui::BeginDisabled(!s.cfgLive);
    ImGui::TextColored(dim, "device");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    const char* devLabel = (s.cfgDevice >= 0 && s.cfgDevice < (int)s.devices.size())
                               ? s.devices[s.cfgDevice].c_str()
                               : "(none)";
    if (ImGui::BeginCombo("##device", devLabel)) {
        for (int i = 0; i < (int)s.devices.size(); ++i) {
            const std::string b = std::to_string(i) + ": " + s.devices[i];
            if (ImGui::Selectable(b.c_str(), i == s.cfgDevice)) s.cfgDevice = i;
        }
        if (s.devices.empty()) ImGui::TextDisabled("no RTL-SDR devices");
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 4);
    if (ImGui::SmallButton("rescan")) s.refreshDevices();
    ImGui::EndDisabled();
}

void drawChannels(AppState& s)
{
    const auto keys = s.keySnapshot();
    const auto counts = s.packets.channelCounts();
    if (ImGui::Selectable("All channels", s.selectedChannel < 0)) s.selectedChannel = -1;
    ImGui::Separator();
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        ImGui::PushID(static_cast<int>(i));
        bool en = k.enabled;
        if (ImGui::Checkbox("##en", &en)) s.setKeyEnabled(i, en);
        ImGui::SameLine(0, 4);
        const std::string label = k.label + "  0x" + hexs(&k.hash, 1);
        const long n = counts.count(k.hash) ? counts.at(k.hash) : 0;
        const std::string row = label + (n ? "   (" + std::to_string(n) + ")" : "");
        if (ImGui::Selectable(row.c_str(), s.selectedChannel == k.hash))
            s.selectedChannel = k.hash;
        ImGui::SameLine();
        ImGui::TextDisabled(k.builtin ? "public" : "user");
        ImGui::PopID();
    }
    std::set<int> known;
    for (const auto& k : keys) known.insert(k.hash);
    for (const auto& kv : counts) {
        if (known.count(kv.first)) continue;
        ImGui::PushID(1000 + kv.first);
        uint8_t h = kv.first;
        const std::string row = "Unknown 0x" + hexs(&h, 1) + "   (" +
                                std::to_string(kv.second) + ")";
        if (ImGui::Selectable(row.c_str(), s.selectedChannel == kv.first))
            s.selectedChannel = kv.first;
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Add and manage keys in the Keys tab.");
}

void drawNodes(AppState& s)
{
    const auto nodes = s.nodes.snapshot();
    if (nodes.empty()) {
        ImGui::TextDisabled("No adverts received yet.");
        return;
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        ImGui::PushID(static_cast<int>(i));
        std::string label = n.name.empty() ? n.pubkeyHex.substr(0, 10) : n.name;
        if (n.hasLocation) label += "  @";
        if (ImGui::Selectable(label.c_str(), s.selectedNode == n.pubkeyHex)) {
            s.selectedNode = n.pubkeyHex;
            if (n.hasLocation) {
                s.map.center_on(n.lat, n.lon, 13);
                s.mapAutoFit = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(n.signatureValid ? green : red, n.signatureValid ? "sig ok" : "sig bad");
        const std::string nt = nodeTypeName(n.nodeType);
        ImGui::TextDisabled("  %s  seen %s  count %d", nt.c_str(),
                            fmtLocalTime(n.lastSeen).c_str(), n.count);
        if (n.hasLocation) ImGui::TextDisabled("  %.6f, %.6f", n.lat, n.lon);
        ImGui::PopID();
    }
}

void drawSidebarImpl(AppState& s)
{
    ImGui::BeginChild("sidebar", ImVec2(360, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTabBar("##sidebarTabs")) {
        if (ImGui::BeginTabItem("Channels")) {
            ImGui::BeginChild("##chscroll", ImVec2(0, 0), ImGuiChildFlags_None);
            drawChannels(s);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Nodes")) {
            ImGui::BeginChild("##nodescroll", ImVec2(0, 0), ImGuiChildFlags_None);
            drawNodes(s);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void drawChat(AppState& s)
{
    const std::string chanLabel =
        s.selectedChannel < 0 ? "All channels"
                              : s.channelLabelFor((uint8_t)s.selectedChannel);
    ImGui::TextColored(text, "%s", chanLabel.c_str());
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##chatfilter", "search messages", s.filters.text,
                             sizeof(s.filters.text));
    ImGui::SameLine();
    ImGui::Checkbox("relays", &s.filters.showRelays);
    ImGui::SameLine();
    if (ImGui::SmallButton(s.paused ? "play" : "pause")) {
        s.paused = !s.paused;
        if (s.paused) s.frozen = s.packets.snapshot(5000);
        else s.frozen.clear();
    }
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, term);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::BeginChild("##chatscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
    const auto view = s.paused ? s.frozen : s.packets.snapshot(5000);
    int shown = 0;
    for (const auto& p : view) {
        if (!p.isText) continue;
        if (s.selectedChannel >= 0 && (int)p.channelHash != s.selectedChannel) continue;
        if (!s.filters.showRelays && p.hops > 0) continue;
        if (s.filters.text[0]) {
            std::string hay = packetSummary(p) + " " + p.sender + " " + p.text + " " +
                              p.channelLabel;
            for (char& c : hay)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            std::string needle = s.filters.text;
            for (char& c : needle)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            if (hay.find(needle) == std::string::npos) continue;
        }
        ++shown;
        ImGui::TextColored(dim, "%s", fmtLocalTime(p.tUnix).c_str());
        ImGui::SameLine(0, 8);
        ImGui::TextColored(cyan, "[%s]", p.channelLabel.empty() ? "?" : p.channelLabel.c_str());
        ImGui::SameLine(0, 8);
        ImGui::TextColored(purple, "%s", p.sender.empty() ? "?" : p.sender.c_str());
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(p.text.c_str());
        if (p.hops > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("h=%d", p.hops);
        }
    }
    if (!shown) ImGui::TextDisabled("No messages yet.");
    if (!s.paused && atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void drawPacketLog(AppState& s)
{
    ImGui::SetNextItemWidth(145);
    const std::string curType =
        s.filters.type < 0 ? "all types" : typeName(s.filters.type);
    if (ImGui::BeginCombo("##ftype", curType.c_str())) {
        if (ImGui::Selectable("all types", s.filters.type < 0)) s.filters.type = -1;
        static const int types[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6,
                                    0x7, 0x8, 0x9, 0xA, 0xB, 0xF};
        for (int t : types) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s (0x%x)", typeName(t).c_str(), t);
            if (ImGui::Selectable(b, s.filters.type == t)) s.filters.type = t;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(130);
    const std::string curRoute =
        s.filters.route < 0 ? "all routes" : routeName(s.filters.route);
    if (ImGui::BeginCombo("##froute", curRoute.c_str())) {
        if (ImGui::Selectable("all routes", s.filters.route < 0)) s.filters.route = -1;
        for (int r = 0; r < 4; ++r) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s (0x%x)", routeName(r).c_str(), r);
            if (ImGui::Selectable(b, s.filters.route == r)) s.filters.route = r;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(150);
    const std::string curCh =
        s.filters.channel < 0 ? "all channels"
                              : s.channelLabelFor((uint8_t)s.filters.channel);
    if (ImGui::BeginCombo("##fch", curCh.c_str())) {
        if (ImGui::Selectable("all channels", s.filters.channel < 0)) s.filters.channel = -1;
        const auto keys = s.keySnapshot();
        std::set<int> seen;
        for (const auto& k : keys) {
            if (!seen.insert(k.hash).second) continue;
            const std::string b = k.label + "  0x" + hexs(&k.hash, 1);
            if (ImGui::Selectable(b.c_str(), s.filters.channel == k.hash))
                s.filters.channel = k.hash;
        }
        for (const auto& kv : s.packets.channelCounts()) {
            if (!seen.insert(kv.first).second) continue;
            char b[64];
            std::snprintf(b, sizeof(b), "Unknown 0x%02x", kv.first);
            if (ImGui::Selectable(b, s.filters.channel == kv.first))
                s.filters.channel = kv.first;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##flog", "filter text", s.filters.text, sizeof(s.filters.text));
    ImGui::SameLine();
    ImGui::Checkbox("relays", &s.filters.showRelays);
    ImGui::SameLine();
    if (ImGui::SmallButton(s.paused ? "play" : "pause")) {
        s.paused = !s.paused;
        if (s.paused) s.frozen = s.packets.snapshot(5000);
        else s.frozen.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear view")) {
        s.packets.clear();
        s.hasSelection = false;
        s.selectedSeq = 0;
        s.frozen.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("CSV")) {
        const std::string p =
            saveFileDialog("Export CSV", "CSV Files\0*.csv\0All Files\0*.*\0", "csv");
        if (!p.empty()) s.exportCsv(p, s.visiblePackets(50000));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("JSON")) {
        const std::string p =
            saveFileDialog("Export JSON", "JSON Files\0*.json\0All Files\0*.*\0", "json");
        if (!p.empty()) s.exportJson(p, s.visiblePackets(50000));
    }
    ImGui::Separator();

    const auto view = s.paused ? s.frozen : s.visiblePackets(3000);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, term);
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
    if (ImGui::BeginTable("##logtable", 8,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("route", ImGuiTableColumnFlags_WidthFixed, 92);
        ImGui::TableSetupColumn("hops", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("q", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableHeadersRow();
        for (const auto& p : view) {
            if (!s.matchFilter(p)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = s.hasSelection && s.selectedSeq == p.seq;
            char row[64];
            std::snprintf(row, sizeof(row), "%s##%llu", fmtLocalTime(p.tUnix).c_str(),
                          (unsigned long long)p.seq);
            if (ImGui::Selectable(row, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                s.selectedPacket = p;
                s.selectedSeq = p.seq;
                s.hasSelection = true;
                s.inspectRequested = true;
            }
            ImGui::TableSetColumnIndex(1);
            const std::string tn = typeName(p.type);
            ImGui::TextColored(packetColor(p.type), "%s", tn.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(routeName(p.route).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", p.hops);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("0x%02x", p.channelHash);
            ImGui::TableSetColumnIndex(5);
            if (p.macOk) ImGui::TextColored(green, "%s", p.channelLabel.c_str());
            else ImGui::TextDisabled("%s", p.channelLabel.empty() ? "-" : p.channelLabel.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(packetSummary(p).c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%.2f", p.quality);
        }
        ImGui::EndTable();
    }
    if (!s.paused && atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawInspector(AppState& s)
{
    if (!s.hasSelection) {
        ImGui::TextDisabled("Select a packet in the log.");
        return;
    }
    const auto& p = s.selectedPacket;
    if (ImGui::BeginTable("##insp", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", v.c_str());
        };
        row("received", fmtShortTime(p.tUnix));
        row("frequency", std::to_string(p.freqHz / 1e6) + " MHz");
        row("type", typeName(p.type));
        row("route", routeName(p.route));
        uint8_t hdr = p.header;
        row("header", "0x" + hexs(&hdr, 1) + "  version " + std::to_string(p.version));
        row("hops", std::to_string(p.hops));
        row("path", hexs(p.path));
        uint8_t ch = p.channelHash;
        row("channel", "0x" + hexs(&ch, 1) + "  " + p.channelLabel);
        row("key/MAC", macStatus(p));
        row("quality", std::to_string(p.quality));
        if (p.isAdvert) {
            row("node", p.advert.name + "  (" + nodeTypeName(p.advert.nodeType) + ")");
            row("pubkey", p.advert.pubkeyHex);
            row("timestamp", fmtUtc(p.advert.timestamp));
            row("signature", p.advert.signatureValid ? "valid" : "INVALID");
            if (p.advert.hasLocation)
                row("location", std::to_string(p.advert.lat) + ", " + std::to_string(p.advert.lon));
        }
        if (p.isText) {
            row("sender", p.sender);
            row("message", p.text);
            row("msg timestamp", fmtUtc(p.msgTimestamp));
        }
        if (p.isData) {
            uint8_t dt[2] = {static_cast<uint8_t>(p.dataType & 0xff),
                             static_cast<uint8_t>(p.dataType >> 8)};
            row("data type", "0x" + hexs(dt, 2));
            row("data", hexs(p.data));
        }
        row("raw", hexs(p.payload));
        ImGui::EndTable();
    }
    if (ImGui::Button("copy raw")) ImGui::SetClipboardText(p.rawHex.c_str());
    if (p.isAdvert) {
        ImGui::SameLine();
        if (ImGui::Button("copy pubkey")) ImGui::SetClipboardText(p.advert.pubkeyHex.c_str());
    }
    ImGui::Spacing();
    std::vector<char> buf(p.rawHex.begin(), p.rawHex.end());
    buf.push_back('\0');
    ImGui::InputTextMultiline("##rawhex", buf.data(), buf.size(), ImVec2(-1, 120),
                              ImGuiInputTextFlags_ReadOnly);
}

void drawStats(AppState& s)
{
    const auto types = s.packets.typeCounts();
    const auto chans = s.packets.channelCounts();
    long group = 0;
    for (const auto& kv : chans) group += kv.second;
    ImGui::Text("packets: %ld    nodes: %zu    group packets: %ld    db rows: %ld",
                s.packets.total(), s.nodes.size(), group, s.store.packetCount());
    ImGui::Separator();
    if (ImGui::BeginTable("##types", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("type");
        ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (const auto& kv : types) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string tn = typeName(kv.first);
            ImGui::TextColored(packetColor(kv.first), "%s", tn.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%ld", kv.second);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("##chans", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("channel");
        ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (const auto& kv : chans) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string cl = s.channelLabelFor(kv.first);
            ImGui::Text("%s  0x%02x", cl.c_str(), kv.first);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%ld", kv.second);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::Text("receiver: %s  freq %.6f MHz  rate %u  floor %.5f",
                s.receiver.running() ? "running" : "stopped", s.status.freqHz / 1e6,
                s.status.rate, s.status.noiseFloor);
    ImGui::Text("triggers %llu  captures %llu  packets %llu  last capture %.2f s  decode %.1f ms",
                s.status.triggers, s.status.captures, s.status.packets, s.status.lastCaptureSec,
                s.status.lastDecodeMs);
    ImGui::Text("db: %s", s.dbPath.c_str());
}

void drawKeys(AppState& s)
{
    auto keys = s.keySnapshot();
    if (ImGui::BeginTable("##keystable", 5,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("label");
        ImGui::TableSetupColumn("hash", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("bytes", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("enabled", ImGuiTableColumnFlags_WidthFixed, 68);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < keys.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(keys[i].label.c_str());
            if (keys[i].builtin) {
                ImGui::SameLine();
                ImGui::TextDisabled("built-in");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%02x", keys[i].hash);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", keys[i].key.size());
            ImGui::TableSetColumnIndex(3);
            bool en = keys[i].enabled;
            ImGui::BeginDisabled(keys[i].builtin);
            if (ImGui::Checkbox(("##en" + std::to_string(i)).c_str(), &en))
                s.setKeyEnabled(i, en);
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(4);
            if (!keys[i].builtin && ImGui::SmallButton(("remove##" + std::to_string(i)).c_str()))
                s.removeKey(i);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextColored(dim, "add a channel key");
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##keylabel", "label (e.g. Family)", s.keyLabel,
                             sizeof(s.keyLabel));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("##keytext", "16/32-byte key as hex or base64", s.keyText,
                             sizeof(s.keyText));
    ImGui::SameLine();
    if (ImGui::Button("add key")) s.addKeyFromEditor();
    if (!s.keyError.empty()) ImGui::TextColored(red, "%s", s.keyError.c_str());
    ImGui::TextDisabled("The public channel key is always enabled. Channel names are not transmitted; labels are local.");
}

void drawSettings(AppState& s)
{
    ImGui::Text("data directory: %s", s.dataDir.c_str());
    ImGui::Text("database: %s", s.dbPath.c_str());
    if (ImGui::Button("open data folder")) {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", s.dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("save settings")) s.saveSettings();
    ImGui::SameLine();
    if (ImGui::Button("clear history...")) s.showClearConfirm = true;
    ImGui::Separator();
    if (ImGui::Checkbox("record packets to SQLite", &s.cfgRecord)) {
        s.recordToDb.store(s.cfgRecord);
        s.saveSettings();
    }
    if (ImGui::Checkbox("verbose receiver log", &s.cfgVerbose)) s.saveSettings();
    if (ImGui::Checkbox("map online", &s.mapOnline)) {
        s.tiles.set_online(s.mapOnline);
        s.saveSettings();
    }
    if (ImGui::Button("apply to running receiver")) s.restartReceiver();
    ImGui::Separator();
    ImGui::TextDisabled("Band presets, frequency, sample rate, gain, ppm, bias tee, threshold, SF and BW are in the toolbar.");
    ImGui::TextDisabled("The GUI is receive-only; the CLI remains MeshCoreSniffer.exe.");
}

void drawEventLog(AppState& s)
{
    if (ImGui::SmallButton("clear log")) s.logs.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("receiver and application diagnostics");
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, term);
    ImGui::BeginChild("##evlog", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
    for (const auto& line : s.logs.snapshot(2000)) ImGui::TextUnformatted(line.c_str());
    if (atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace

void drawSidebar(AppState& s) { drawSidebarImpl(s); }

void drawMenuBar(AppState& s)
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open IQ WAV...")) {
            const std::string p =
                openFileDialog("Open IQ WAV", "WAV Files\0*.wav\0All Files\0*.*\0", "wav");
            if (!p.empty()) {
                std::snprintf(s.cfgWavPath, sizeof(s.cfgWavPath), "%s", p.c_str());
                s.cfgLive = false;
                s.saveSettings();
                s.statusMsg = "WAV selected";
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export CSV...")) {
            const std::string p =
                saveFileDialog("Export CSV", "CSV Files\0*.csv\0All Files\0*.*\0", "csv");
            if (!p.empty()) s.exportCsv(p, s.visiblePackets(50000));
        }
        if (ImGui::MenuItem("Export JSON...")) {
            const std::string p =
                saveFileDialog("Export JSON", "JSON Files\0*.json\0All Files\0*.*\0", "json");
            if (!p.empty()) s.exportJson(p, s.visiblePackets(50000));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) s.requestQuit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Source")) {
        if (ImGui::MenuItem("Start")) s.startReceiver();
        if (ImGui::MenuItem("Stop", nullptr, false, s.receiver.running())) s.stopReceiver();
        if (ImGui::MenuItem("Restart", nullptr, false, s.receiver.running())) s.restartReceiver();
        ImGui::Separator();
        if (ImGui::MenuItem("RTL-SDR", nullptr, s.cfgLive)) {
            s.cfgLive = true;
            s.saveSettings();
        }
        if (ImGui::MenuItem("IQ WAV", nullptr, !s.cfgLive)) {
            s.cfgLive = false;
            s.saveSettings();
        }
        if (ImGui::MenuItem("Rescan devices")) s.refreshDevices();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Clear history...")) s.showClearConfirm = true;
        if (ImGui::MenuItem("Clear event log")) s.logs.clear();
        if (ImGui::MenuItem("Open data folder")) {
#ifdef _WIN32
            ShellExecuteA(nullptr, "open", s.dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) s.showAbout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void drawHeader(AppState& s)
{
    ImGui::BeginChild("header", ImVec2(0, 74), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginChild("hleft", ImVec2(std::max(300.0f, ImGui::GetContentRegionAvail().x - 210),
                                      0),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(text, "MeshCoreSniffer");
    ImGui::SameLine(0, 10);
    ImGui::TextDisabled("MeshCore LoRa receiver");
    ImGui::Spacing();
    const bool running = s.receiver.running();
    theme::chip(running ? "RUNNING" : "STOPPED", running ? green : dim,
                running ? ImVec4(0.05f, 0.20f, 0.12f, 1.0f) : slot);
    theme::chip(s.cfgLive ? "RTL-SDR" : "IQ WAV", cyan, slot);
    char b[64];
    std::snprintf(b, sizeof(b), "%.6f MHz", s.cfgFreqMhz);
    theme::chip(b, text, slot);
    std::snprintf(b, sizeof(b), "%u Hz", s.cfgRate);
    theme::chip(b, text, slot);
    std::snprintf(b, sizeof(b), "%zu keys", s.keySnapshot().size());
    theme::chip(b, text, slot);
    std::snprintf(b, sizeof(b), "%ld packets", s.packets.total());
    theme::chip(b, green, slot);
    std::snprintf(b, sizeof(b), "%zu nodes", s.nodes.size());
    theme::chip(b, purple, slot);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("hright", ImVec2(200, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    const std::string clock = fmtLocalTime((double)std::time(nullptr));
    ImGui::TextColored(text, "%s", clock.c_str());
    ImGui::TextDisabled("local time");
    ImGui::EndChild();
    ImGui::EndChild();
}

void drawToolbar(AppState& s)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
    ImGui::BeginChild("toolbar", ImVec2(0, 84), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    draw_source_row(s);
    ImGui::Spacing();
    draw_band_preset(s);
    ImGui::SameLine(0, 8);
    draw_freq_editor(s);
    ImGui::SameLine(0, 8);
    draw_rate(s);
    ImGui::SameLine(0, 8);
    draw_gain(s);
    ImGui::SameLine(0, 8);
    ImGui::TextColored(dim, "ppm");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(54);
    ImGui::InputInt("##ppm", &s.cfgPpm);
    ImGui::SameLine(0, 8);
    ImGui::Checkbox("bias tee", &s.cfgBiasTee);
    ImGui::SameLine(0, 8);
    ImGui::TextColored(dim, "trigger");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    float thr = (float)s.cfgThreshold;
    if (ImGui::SliderFloat("##thr", &thr, 1.0f, 20.0f, "%.1f dB",
                           ImGuiSliderFlags_AlwaysClamp))
        s.cfgThreshold = thr;
    ImGui::SameLine(0, 8);
    draw_sf_bw(s);
    ImGui::Spacing();
    if (!s.cfgLive) {
        ImGui::TextColored(dim, "wav");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(460);
        ImGui::InputText("##wavpath", s.cfgWavPath, sizeof(s.cfgWavPath));
        ImGui::SameLine();
        if (ImGui::Button("browse...")) {
            const std::string p =
                openFileDialog("Open IQ WAV", "WAV Files\0*.wav\0All Files\0*.*\0", "wav");
            if (!p.empty()) std::snprintf(s.cfgWavPath, sizeof(s.cfgWavPath), "%s", p.c_str());
        }
        ImGui::SameLine();
        ImGui::Checkbox("decode all", &s.cfgDecodeAll);
    } else {
        ImGui::TextDisabled("Live RTL-SDR. Press Start/Restart to apply configuration changes.");
    }
    if (!s.statusMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(amber, "  %s", s.statusMsg.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void drawMapPanel(AppState& s, const ImVec2& size)
{
    if (theme::begin_card("mapcard", size)) {
        std::vector<MapMarker> markers;
        const auto nodes = s.nodes.snapshot();
        const double now = (double)std::time(nullptr);
        for (const auto& n : nodes) {
            if (!n.hasLocation) continue;
            MapMarker m;
            m.lat = n.lat;
            m.lon = n.lon;
            m.id = n.pubkeyHex;
            m.label = n.name.empty() ? n.pubkeyHex.substr(0, 8) : n.name;
            m.fresh = (now - n.lastSeen) < 180.0;
            m.selected = (n.pubkeyHex == s.selectedNode);
            markers.push_back(std::move(m));
        }
        if (s.mapAutoFit && !markers.empty()) {
            s.map.fit(markers);
            s.mapAutoFit = false;
        }
        ImGui::TextColored(text, "NODE MAP");
        ImGui::SameLine(0, 16);
        if (ImGui::SmallButton("+")) s.map.zoom_in();
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("-")) s.map.zoom_out();
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("fit")) s.map.fit(markers);
        ImGui::SameLine(0, 16);
        bool online = s.mapOnline;
        if (ImGui::Checkbox("online", &online)) {
            s.mapOnline = online;
            s.tiles.set_online(online);
            s.saveSettings();
        }
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("%zu located   z=%d   tiles %d ready / %d pending / %d failed",
                            markers.size(), s.map.zoom(), s.tiles.ready(), s.tiles.pending(),
                            s.tiles.failed());
        ImGui::Spacing();
        if (markers.empty())
            ImGui::TextDisabled("No located adverts yet. Adverts with latitude/longitude appear here.");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        std::string clicked;
        if (s.map.draw(s.tiles, avail, markers, clicked)) {
            s.selectedNode = clicked;
            NodeRecord n;
            if (s.nodes.get(clicked, n) && n.hasLocation)
                s.map.center_on(n.lat, n.lon, std::max(12, s.map.zoom()));
        }
    }
    theme::end_card();
}

void drawMainTabs(AppState& s)
{
    if (theme::begin_card("maincard", ImVec2(0, 0))) {
        if (ImGui::BeginTabBar("##mainTabs")) {
            if (ImGui::BeginTabItem("Chat")) {
                drawChat(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Packet Log")) {
                drawPacketLog(s);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags inspectFlags =
                s.inspectRequested ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Inspector", nullptr, inspectFlags)) {
                drawInspector(s);
                s.inspectRequested = false;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Statistics")) {
                drawStats(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Keys")) {
                drawKeys(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                drawSettings(s);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Event Log")) {
                drawEventLog(s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    theme::end_card();
}

void drawStatusBar(AppState& s)
{
    ImGui::Separator();
    ImGui::TextColored(s.lastError.empty() ? dim : red, "%s",
                       s.lastError.empty() ? s.statusMsg.c_str() : s.lastError.c_str());
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled(
        "floor %.5f  capture %.2fs  decode %.1fms  triggers %llu  captures %llu  packets %llu",
        s.status.noiseFloor, s.status.lastCaptureSec, s.status.lastDecodeMs, s.status.triggers,
        s.status.captures, s.status.packets);
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("%s", s.dbPath.c_str());
}

void drawModals(AppState& s)
{
    if (s.showAbout) {
        ImGui::OpenPopup("About MeshCoreSniffer");
        s.showAbout = false;
    }
    if (ImGui::BeginPopupModal("About MeshCoreSniffer", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(kAboutText);
        ImGui::Spacing();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s.showClearConfirm) {
        ImGui::OpenPopup("Clear history?");
        s.showClearConfirm = false;
    }
    if (ImGui::BeginPopupModal("Clear history?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete all stored packets and adverts? Keys and settings are kept.");
        ImGui::Spacing();
        if (ImGui::Button("Clear")) {
            s.clearHistory();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace mcs::gui
