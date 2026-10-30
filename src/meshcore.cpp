// meshcore.cpp - MeshCore v1 packet parsing and group-channel decryption.

#include "meshcore.h"
#include "crypto.h"
#include "ed_25519.h"

#include <algorithm>
#include <cstring>

namespace mc {
namespace {

constexpr uint8_t PH_ROUTE_MASK = 0x03;
constexpr uint8_t PH_TYPE_SHIFT = 2;
constexpr uint8_t PH_TYPE_MASK = 0x0F;
constexpr uint8_t PH_VER_SHIFT = 6;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

} // namespace

const char* meshcoreRouteName(int route)
{
    switch (route) {
        case 0: return "TRANSPORT_FLOOD";
        case 1: return "FLOOD";
        case 2: return "DIRECT";
        case 3: return "TRANSPORT_DIRECT";
        default: return "?";
    }
}

const char* meshcorePayloadName(int type)
{
    switch (type) {
        case 0x0: return "REQ";
        case 0x1: return "RESPONSE";
        case 0x2: return "TXT_MSG";
        case 0x3: return "ACK";
        case 0x4: return "ADVERT";
        case 0x5: return "GRP_TXT";
        case 0x6: return "GRP_DATA";
        case 0x7: return "ANON_REQ";
        case 0x8: return "PATH";
        case 0x9: return "TRACE";
        case 0xA: return "MULTIPART";
        case 0xB: return "CONTROL";
        case 0xF: return "RAW_CUSTOM";
        default: return "RESERVED";
    }
}

bool parseMeshCore(const std::vector<uint8_t>& data, MeshCorePacket& p, std::string& err)
{
    p = MeshCorePacket{};
    if (data.empty()) { err = "empty MeshCore packet"; return false; }

    size_t i = 0;
    p.header = data[i++];
    p.route = p.header & PH_ROUTE_MASK;
    p.type = (p.header >> PH_TYPE_SHIFT) & PH_TYPE_MASK;
    p.version = (p.header >> PH_VER_SHIFT) & 0x03;

    p.hasTransportCodes = (p.route == 0 || p.route == 3);
    if (p.hasTransportCodes) {
        if (i + 4 > data.size()) { err = "truncated transport codes"; return false; }
        p.transportCodes[0] = rd16(&data[i]);
        p.transportCodes[1] = rd16(&data[i + 2]);
        i += 4;
    }

    if (i >= data.size()) { err = "missing path length"; return false; }
    p.pathLenByte = data[i++];
    p.hashSize = (p.pathLenByte >> 6) + 1;
    p.hashCount = p.pathLenByte & 0x3F;
    if (p.hashSize == 4) { err = "reserved path hash size"; return false; }

    const size_t pathBytes = static_cast<size_t>(p.hashSize) * p.hashCount;
    if (i + pathBytes > data.size()) { err = "truncated path"; return false; }
    p.path.assign(data.begin() + static_cast<std::ptrdiff_t>(i),
                  data.begin() + static_cast<std::ptrdiff_t>(i + pathBytes));
    i += pathBytes;

    if (i > data.size()) { err = "truncated payload"; return false; }
    p.payloadOffset = i;
    p.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(i), data.end());
    return true;
}

const char* meshcoreNodeTypeName(int type)
{
    switch (type) {
        case 1: return "chat";
        case 2: return "repeater";
        case 3: return "room";
        case 4: return "sensor";
        default: return "node";
    }
}

bool parseAdvert(const std::vector<uint8_t>& payload, MeshCoreAdvert& a)
{
    a = MeshCoreAdvert{};
    if (payload.size() < 100 + 1) return false; // pubkey + ts + sig + flags

    std::memcpy(a.publicKey, payload.data(), 32);
    a.timestamp = static_cast<uint32_t>(payload[32]) |
                  (static_cast<uint32_t>(payload[33]) << 8) |
                  (static_cast<uint32_t>(payload[34]) << 16) |
                  (static_cast<uint32_t>(payload[35]) << 24);
    std::memcpy(a.signature, payload.data() + 36, 64);

    a.flags = payload[100];
    a.nodeType = a.flags & 0x0F;
    size_t i = 101;

    if (a.flags & 0x10) {
        if (i + 8 > payload.size()) return false;
        a.hasLocation = true;
        a.latitude = static_cast<int32_t>(
            static_cast<uint32_t>(payload[i]) |
            (static_cast<uint32_t>(payload[i + 1]) << 8) |
            (static_cast<uint32_t>(payload[i + 2]) << 16) |
            (static_cast<uint32_t>(payload[i + 3]) << 24));
        i += 4;
        a.longitude = static_cast<int32_t>(
            static_cast<uint32_t>(payload[i]) |
            (static_cast<uint32_t>(payload[i + 1]) << 8) |
            (static_cast<uint32_t>(payload[i + 2]) << 16) |
            (static_cast<uint32_t>(payload[i + 3]) << 24));
        i += 4;
    }
    if (a.flags & 0x20) i += 2; // feature 1
    if (a.flags & 0x40) i += 2; // feature 2
    if (a.flags & 0x80) {
        if (i > payload.size()) return false;
        a.name.assign(reinterpret_cast<const char*>(payload.data() + i),
                      payload.size() - i);
        const size_t nul = a.name.find('\0');
        if (nul != std::string::npos) a.name.resize(nul);
    }

    // Signature covers pubkey || timestamp || appdata.
    std::vector<uint8_t> signedMsg(payload.begin(), payload.begin() + 36);
    signedMsg.insert(signedMsg.end(), payload.begin() + 100, payload.end());
    a.signatureValid = ed25519_verify(a.signature, signedMsg.data(), signedMsg.size(),
                                      a.publicKey) == 1;
    return true;
}

bool isMeshCoreV1(const MeshCorePacket& p)
{
    if (p.version != 0) return false;
    // Known MeshCore v1 payload types (0x0C..0x0E are reserved).
    if (!(p.type <= 0x0B || p.type == 0x0F)) return false;
    if ((p.type == 0x05 || p.type == 0x06) && p.payload.size() < 3) return false;
    return true;
}

bool decryptGroupPayload(const MeshCorePacket& p,
                         const std::vector<uint8_t>& key,
                         std::vector<uint8_t>& plaintext,
                         bool& macOk,
                         uint8_t& expectedChannelHash)
{
    plaintext.clear();
    macOk = false;

    if (key.size() != 16 && key.size() != 32) return false;
    if (p.type != 0x05 && p.type != 0x06) return false;
    if (p.payload.size() < 3) return false;

    const auto hash = sha256(key.data(), key.size());
    expectedChannelHash = hash[0];

    if (p.payload[0] != expectedChannelHash) return false;

    const uint8_t mac0 = p.payload[1];
    const uint8_t mac1 = p.payload[2];
    const std::vector<uint8_t> ct(p.payload.begin() + 3, p.payload.end());
    if (ct.empty() || (ct.size() % 16) != 0) return false;

    uint8_t secret[32] = {};
    std::memcpy(secret, key.data(), key.size());

    uint8_t hmac[32];
    hmacSha256(secret, sizeof(secret), ct.data(), ct.size(), hmac);
    macOk = (hmac[0] == mac0 && hmac[1] == mac1);
    if (!macOk) return false;

    Aes128 aes;
    aes.setKey(secret);
    plaintext.resize(ct.size());
    for (size_t off = 0; off < ct.size(); off += 16)
        aes.decryptBlock(&ct[off], &plaintext[off]);

    while (!plaintext.empty() && plaintext.back() == 0) plaintext.pop_back();
    return true;
}

bool parseGroupText(const std::vector<uint8_t>& plaintext,
                    uint32_t& timestamp, uint8_t& flags, std::string& text)
{
    if (plaintext.size() < 5) return false;
    timestamp = static_cast<uint32_t>(plaintext[0]) |
                (static_cast<uint32_t>(plaintext[1]) << 8) |
                (static_cast<uint32_t>(plaintext[2]) << 16) |
                (static_cast<uint32_t>(plaintext[3]) << 24);
    flags = plaintext[4];
    text.assign(reinterpret_cast<const char*>(plaintext.data() + 5), plaintext.size() - 5);
    const size_t nul = text.find('\0');
    if (nul != std::string::npos) text.resize(nul);
    return true;
}

bool parseGroupData(const std::vector<uint8_t>& plaintext,
                    uint16_t& dataType, std::vector<uint8_t>& data)
{
    if (plaintext.size() < 3) return false;
    dataType = rd16(plaintext.data());
    const size_t len = std::min<size_t>(plaintext[2], plaintext.size() - 3);
    data.assign(plaintext.begin() + 3, plaintext.begin() + 3 + static_cast<std::ptrdiff_t>(len));
    return true;
}

} // namespace mc
