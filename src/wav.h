// wav.h - minimal RIFF/WAVE reader for 16-bit IQ recordings (I=L, Q=R).
#pragma once

#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace mc {

struct WavData {
    double sampleRate = 0.0;
    int channels = 0;
    std::vector<std::complex<double>> iq; // I = left, Q = right
};

namespace detail {

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace detail

namespace detail {
inline void wr16(std::ofstream& f, uint16_t v)
{
    const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
    f.write(b, 2);
}
inline void wr32(std::ofstream& f, uint32_t v)
{
    const char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                       static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff)};
    f.write(b, 4);
}
inline int16_t toPcm16(double v)
{
    long x = std::llround(v * 32768.0);
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return static_cast<int16_t>(x);
}
} // namespace detail

// Write a 16-bit stereo IQ WAV (I=left, Q=right) for diagnostics.
inline bool writeWavIq(const std::string& path,
                       const std::vector<std::complex<double>>& iq,
                       double sampleRate, std::string& err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }

    const uint32_t dataBytes = static_cast<uint32_t>(iq.size() * 4);
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * 4;
    f.write("RIFF", 4);
    detail::wr32(f, 36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    detail::wr32(f, 16);
    detail::wr16(f, 1);
    detail::wr16(f, 2);
    detail::wr32(f, static_cast<uint32_t>(sampleRate));
    detail::wr32(f, byteRate);
    detail::wr16(f, 4);
    detail::wr16(f, 16);
    f.write("data", 4);
    detail::wr32(f, dataBytes);

    for (const auto& s : iq) {
        detail::wr16(f, static_cast<uint16_t>(detail::toPcm16(s.real())));
        detail::wr16(f, static_cast<uint16_t>(detail::toPcm16(s.imag())));
    }
    if (!f) { err = "write failed: " + path; return false; }
    return true;
}

inline bool readWav(const std::string& path, WavData& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }

    uint8_t hdr[12];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    int audioFormat = 0, bitsPerSample = 0;
    int channels = 0;
    uint32_t sampleRate = 0;
    bool haveFmt = false;

    while (f) {
        uint8_t ch[8];
        f.read(reinterpret_cast<char*>(ch), sizeof(ch));
        if (!f) break;
        const uint32_t chunkSize = detail::rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmt(chunkSize);
            f.read(reinterpret_cast<char*>(fmt.data()), chunkSize);
            if (!f || chunkSize < 16) { err = "bad fmt chunk"; return false; }
            audioFormat = detail::rd16(&fmt[0]);
            channels = detail::rd16(&fmt[2]);
            sampleRate = detail::rd32(&fmt[4]);
            bitsPerSample = detail::rd16(&fmt[14]);
            haveFmt = true;
            if (chunkSize & 1) f.seekg(1, std::ios::cur); // RIFF padding
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (!haveFmt) { err = "data before fmt"; return false; }
            if (audioFormat != 1 || bitsPerSample != 16) {
                err = "only 16-bit PCM is supported";
                return false;
            }
            if (channels < 1 || channels > 2) {
                err = "only mono/stereo is supported";
                return false;
            }

            std::vector<uint8_t> bytes(chunkSize);
            f.read(reinterpret_cast<char*>(bytes.data()), chunkSize);
            const size_t gotBytes = static_cast<size_t>(f.gcount());
            const size_t got = gotBytes / 2;

            auto sample = [&bytes](size_t i) -> int16_t {
                return static_cast<int16_t>(detail::rd16(&bytes[2 * i]));
            };

            if (channels == 2) {
                out.iq.reserve(out.iq.size() + got / 2);
                for (size_t i = 0; i + 1 < got; i += 2) {
                    out.iq.emplace_back(sample(i) / 32768.0, sample(i + 1) / 32768.0);
                }
            } else {
                out.iq.reserve(out.iq.size() + got);
                for (size_t i = 0; i < got; ++i)
                    out.iq.emplace_back(sample(i) / 32768.0, 0.0);
            }
            if (chunkSize & 1) f.seekg(1, std::ios::cur);
        } else {
            f.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
        }
    }

    if (out.iq.empty()) { err = "no audio data"; return false; }
    out.sampleRate = static_cast<double>(sampleRate);
    out.channels = channels;
    return true;
}

} // namespace mc
