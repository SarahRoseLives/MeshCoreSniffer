// rtl_sdr.h - small runtime-loaded wrapper around librtlsdr.
//
// The library is loaded dynamically so the file-decoding mode keeps working on
// systems without an RTL-SDR / librtlsdr installation.
#pragma once

#include <cstdint>
#include <string>

namespace mc {

class RtlSdr {
public:
    RtlSdr() = default;
    ~RtlSdr();

    RtlSdr(const RtlSdr&) = delete;
    RtlSdr& operator=(const RtlSdr&) = delete;

    bool open(int index, std::string& err);
    bool configure(double freqHz, uint32_t sampleRate, const std::string& gain,
                   int ppm, bool biasTee, std::string& err);
    int read(uint8_t* buf, int len, int& nRead, std::string& err);

    void close();
    bool isOpen() const;
    std::string deviceName() const;
    uint32_t actualCenterFreq() const;
    uint32_t actualSampleRate() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace mc
