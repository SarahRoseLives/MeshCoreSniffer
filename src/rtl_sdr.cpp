// rtl_sdr.cpp - runtime-loaded librtlsdr wrapper.

#include "rtl_sdr.h"

#include <cmath>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct rtlsdr_dev;

namespace mc {
namespace {

using get_count_fn = uint32_t (*)();
using get_name_fn = const char* (*)(uint32_t);
using open_fn = int (*)(rtlsdr_dev**, uint32_t);
using close_fn = int (*)(rtlsdr_dev*);
using set_sample_rate_fn = int (*)(rtlsdr_dev*, uint32_t);
using get_sample_rate_fn = uint32_t (*)(rtlsdr_dev*);
using set_tuner_bandwidth_fn = int (*)(rtlsdr_dev*, uint32_t);
using set_center_freq_fn = int (*)(rtlsdr_dev*, uint32_t);
using get_center_freq_fn = uint32_t (*)(rtlsdr_dev*);
using set_freq_correction_fn = int (*)(rtlsdr_dev*, int);
using set_tuner_gain_mode_fn = int (*)(rtlsdr_dev*, int);
using set_tuner_gain_fn = int (*)(rtlsdr_dev*, int);
using set_agc_mode_fn = int (*)(rtlsdr_dev*, int);
using set_direct_sampling_fn = int (*)(rtlsdr_dev*, int);
using set_offset_tuning_fn = int (*)(rtlsdr_dev*, int);
using set_bias_tee_fn = int (*)(rtlsdr_dev*, int);
using reset_buffer_fn = int (*)(rtlsdr_dev*);
using read_sync_fn = int (*)(rtlsdr_dev*, void*, int, int*);

} // namespace

struct RtlSdr::Impl {
    void* lib = nullptr;
    rtlsdr_dev* dev = nullptr;
    std::string name;
    uint32_t actualFreq = 0;
    uint32_t actualRate = 0;

    get_count_fn get_count = nullptr;
    get_name_fn get_name = nullptr;
    open_fn open_dev = nullptr;
    close_fn close_dev = nullptr;
    set_sample_rate_fn set_sample_rate = nullptr;
    get_sample_rate_fn get_sample_rate = nullptr;
    set_tuner_bandwidth_fn set_tuner_bandwidth = nullptr;
    set_center_freq_fn set_center_freq = nullptr;
    get_center_freq_fn get_center_freq = nullptr;
    set_freq_correction_fn set_freq_correction = nullptr;
    set_tuner_gain_mode_fn set_tuner_gain_mode = nullptr;
    set_tuner_gain_fn set_tuner_gain = nullptr;
    set_agc_mode_fn set_agc_mode = nullptr;
    set_direct_sampling_fn set_direct_sampling = nullptr;
    set_offset_tuning_fn set_offset_tuning = nullptr;
    set_bias_tee_fn set_bias_tee = nullptr;
    reset_buffer_fn reset_buffer = nullptr;
    read_sync_fn read_sync = nullptr;

    void* sym(const char* name) const
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
        return dlsym(lib, name);
#endif
    }

    bool load(std::string& err)
    {
#ifdef _WIN32
        const char* candidates[] = {
            "librtlsdr.dll",                          // portable release folder
            "C:\\msys64\\mingw64\\bin\\librtlsdr.dll",
            "rtlsdr.dll",
        };
        for (const char* path : candidates) {
            HMODULE h = nullptr;
            if (std::strchr(path, '\\') || std::strchr(path, '/'))
                h = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            else
                h = LoadLibraryA(path);
            if (h) { lib = h; break; }
        }
        if (!lib) {
            err = "librtlsdr.dll not found (install mingw-w64-x86_64-rtl-sdr or "
                  "add C:\\msys64\\mingw64\\bin to PATH)";
            return false;
        }
#else
        const char* candidates[] = {"librtlsdr.so.0", "librtlsdr.so"};
        for (const char* path : candidates) {
            lib = dlopen(path, RTLD_NOW);
            if (lib) break;
        }
        if (!lib) { err = "librtlsdr shared library not found"; return false; }
#endif

#define MC_LOAD_REQ(field, symbol)                                            \
    do {                                                                      \
        field = reinterpret_cast<decltype(field)>(sym(symbol));               \
        if (!field) { err = std::string("librtlsdr: missing ") + symbol;      \
                      return false; }                                         \
    } while (0)
#define MC_LOAD_OPT(field, symbol)                                            \
    do { field = reinterpret_cast<decltype(field)>(sym(symbol)); } while (0)

        MC_LOAD_REQ(get_count, "rtlsdr_get_device_count");
        MC_LOAD_REQ(get_name, "rtlsdr_get_device_name");
        MC_LOAD_REQ(open_dev, "rtlsdr_open");
        MC_LOAD_REQ(close_dev, "rtlsdr_close");
        MC_LOAD_REQ(set_sample_rate, "rtlsdr_set_sample_rate");
        MC_LOAD_REQ(set_center_freq, "rtlsdr_set_center_freq");
        MC_LOAD_REQ(set_tuner_gain_mode, "rtlsdr_set_tuner_gain_mode");
        MC_LOAD_REQ(set_tuner_gain, "rtlsdr_set_tuner_gain");
        MC_LOAD_REQ(reset_buffer, "rtlsdr_reset_buffer");
        MC_LOAD_REQ(read_sync, "rtlsdr_read_sync");

        MC_LOAD_OPT(get_sample_rate, "rtlsdr_get_sample_rate");
        MC_LOAD_OPT(set_tuner_bandwidth, "rtlsdr_set_tuner_bandwidth");
        MC_LOAD_OPT(get_center_freq, "rtlsdr_get_center_freq");
        MC_LOAD_OPT(set_freq_correction, "rtlsdr_set_freq_correction");
        MC_LOAD_OPT(set_agc_mode, "rtlsdr_set_agc_mode");
        MC_LOAD_OPT(set_direct_sampling, "rtlsdr_set_direct_sampling");
        MC_LOAD_OPT(set_offset_tuning, "rtlsdr_set_offset_tuning");
        MC_LOAD_OPT(set_bias_tee, "rtlsdr_set_bias_tee");
#undef MC_LOAD_REQ
#undef MC_LOAD_OPT
        return true;
    }

    void unload()
    {
        if (!lib) return;
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
        dlclose(lib);
#endif
        lib = nullptr;
    }
};

RtlSdr::~RtlSdr() { close(); }

bool RtlSdr::open(int index, std::string& err)
{
    close();
    impl_ = new Impl();
    if (!impl_->load(err)) { close(); return false; }

    const uint32_t count = impl_->get_count();
    if (count == 0) { err = "no RTL-SDR devices found"; close(); return false; }
    if (index < 0 || static_cast<uint32_t>(index) >= count) {
        err = "RTL-SDR device index " + std::to_string(index) + " out of range (found " +
              std::to_string(count) + ")";
        close();
        return false;
    }

    const int rc = impl_->open_dev(&impl_->dev, static_cast<uint32_t>(index));
    if (rc != 0) {
        err = "rtlsdr_open failed (" + std::to_string(rc) + ")";
        close();
        return false;
    }

    const char* n = impl_->get_name(static_cast<uint32_t>(index));
    impl_->name = n ? n : "RTL-SDR";
    return true;
}

bool RtlSdr::configure(double freqHz, uint32_t sampleRate, const std::string& gain,
                       int ppm, bool biasTee, std::string& err)
{
    if (!impl_ || !impl_->dev) { err = "RTL-SDR is not open"; return false; }

    int rc = impl_->set_sample_rate(impl_->dev, sampleRate);
    if (rc != 0) {
        err = "rtlsdr_set_sample_rate(" + std::to_string(sampleRate) +
              ") failed (" + std::to_string(rc) + ")";
        return false;
    }
    impl_->actualRate = impl_->get_sample_rate ? impl_->get_sample_rate(impl_->dev) : sampleRate;

    // Match rtl_sdr's configuration order: size the tuner IF filter to the
    // sample rate before tuning and configuring gain.
    if (impl_->set_tuner_bandwidth) impl_->set_tuner_bandwidth(impl_->dev, 0);

    if (impl_->set_direct_sampling) impl_->set_direct_sampling(impl_->dev, 0);
    if (impl_->set_offset_tuning) impl_->set_offset_tuning(impl_->dev, 0);
    if (impl_->set_agc_mode) impl_->set_agc_mode(impl_->dev, 0);

    if (ppm != 0 && impl_->set_freq_correction)
        impl_->set_freq_correction(impl_->dev, ppm);

    if (biasTee) {
        if (!impl_->set_bias_tee) {
            err = "this librtlsdr build does not support bias tee control";
            return false;
        }
        impl_->set_bias_tee(impl_->dev, 1);
    }

    // Tune before configuring gain, matching rtl_sdr.  Some tuners only settle
    // their gain mode reliably once the LO is locked.
    const uint32_t center = static_cast<uint32_t>(std::llround(freqHz));
    rc = impl_->set_center_freq(impl_->dev, center);
    if (rc != 0) {
        err = "rtlsdr_set_center_freq(" + std::to_string(center) +
              ") failed (" + std::to_string(rc) + ")";
        return false;
    }
    impl_->actualFreq = impl_->get_center_freq ? impl_->get_center_freq(impl_->dev) : center;

    const bool autoGain = gain.empty() || gain == "auto" || gain == "AUTO";
    if (autoGain) {
        // librtlsdr API: mode 0 = automatic gain, mode 1 = manual gain.
        impl_->set_tuner_gain_mode(impl_->dev, 0);
    } else {
        double db = 0.0;
        try {
            db = std::stod(gain);
        } catch (...) {
            err = "invalid gain value '" + gain + "' (use auto or dB)";
            return false;
        }
        impl_->set_tuner_gain_mode(impl_->dev, 1);
        impl_->set_tuner_gain(impl_->dev, static_cast<int>(std::llround(db * 10.0)));
    }

    impl_->reset_buffer(impl_->dev);
    return true;
}

int RtlSdr::read(uint8_t* buf, int len, int& nRead, std::string& err)
{
    nRead = 0;
    if (!impl_ || !impl_->dev) { err = "RTL-SDR is not open"; return -1; }
    const int rc = impl_->read_sync(impl_->dev, buf, len, &nRead);
    if (rc != 0) {
        err = "rtlsdr_read_sync failed (" + std::to_string(rc) + ")";
        return rc;
    }
    return 0;
}

void RtlSdr::close()
{
    if (!impl_) return;
    if (impl_->dev && impl_->close_dev) impl_->close_dev(impl_->dev);
    impl_->unload();
    delete impl_;
    impl_ = nullptr;
}

bool RtlSdr::isOpen() const { return impl_ && impl_->dev; }

std::string RtlSdr::deviceName() const { return impl_ ? impl_->name : std::string(); }

uint32_t RtlSdr::actualCenterFreq() const
{
    return impl_ ? impl_->actualFreq : 0;
}

uint32_t RtlSdr::actualSampleRate() const
{
    return impl_ ? impl_->actualRate : 0;
}

} // namespace mc
