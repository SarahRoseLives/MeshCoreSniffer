// meshcore.h - MeshCore v1 packet parsing and public-channel decryption.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

struct MeshCorePacket {
    uint8_t header = 0;
    uint8_t version = 0;
    uint8_t route = 0;
    uint8_t type = 0;

    bool hasTransportCodes = false;
    uint16_t transportCodes[2] = {0, 0};

    uint8_t pathLenByte = 0;
    int hashSize = 1;
    int hashCount = 0;
    std::vector<uint8_t> path;

    std::vector<uint8_t> payload;
    size_t payloadOffset = 0;
};

struct MeshCoreAdvert {
    uint8_t publicKey[32] = {};
    uint32_t timestamp = 0;
    uint8_t signature[64] = {};
    uint8_t flags = 0;
    int nodeType = 0;          // 1=chat, 2=repeater, 3=room, 4=sensor
    bool hasLocation = false;
    int32_t latitude = 0;      // * 1e6
    int32_t longitude = 0;     // * 1e6
    std::string name;
    bool signatureValid = false;
};

const char* meshcoreRouteName(int route);
const char* meshcorePayloadName(int type);
const char* meshcoreNodeTypeName(int type);

bool parseMeshCore(const std::vector<uint8_t>& data, MeshCorePacket& p, std::string& err);

// Sanity check for a MeshCore v1 packet (rejects false CRC hits).
bool isMeshCoreV1(const MeshCorePacket& p);

// Decode a PAYLOAD_TYPE_ADVERT payload (public key, timestamp, signature,
// appdata including node name/location).  Signature verification is optional
// and done separately.
bool parseAdvert(const std::vector<uint8_t>& payload, MeshCoreAdvert& a);

// Try to decrypt a GRP_TXT / GRP_DATA payload with one channel key.
// keyLen must be 16 or 32 bytes (MeshCore accepts both).
bool decryptGroupPayload(const MeshCorePacket& p,
                         const std::vector<uint8_t>& key,
                         std::vector<uint8_t>& plaintext,
                         bool& macOk,
                         uint8_t& expectedChannelHash);

// GRP_TXT plaintext: timestamp(4 LE) + flags(1) + "name: message\0"
bool parseGroupText(const std::vector<uint8_t>& plaintext,
                    uint32_t& timestamp, uint8_t& flags, std::string& text);

// GRP_DATA plaintext: data_type(2 LE) + data_len(1) + data
bool parseGroupData(const std::vector<uint8_t>& plaintext,
                    uint16_t& dataType, std::vector<uint8_t>& data);

} // namespace mc
