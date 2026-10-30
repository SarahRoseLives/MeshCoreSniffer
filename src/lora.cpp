// lora.cpp - LoRa PHY demodulator and FEC for MeshCore captures.
//
// The DSP/FEC chain mirrors the well-known gr-lora_sdr implementation:
//   dechirp -> FFT -> (bin-1) -> Gray -> deinterleave -> Hamming -> dewhiten.
// The sync word is LoRa private (0x12) and the explicit header carries the
// payload length, coding rate and CRC flag.

#include "lora.h"
#include "fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>

namespace mc {
namespace {

struct Ctx {
    const LoraParams& p;
    std::vector<std::complex<double>> up; // reference up-chirp at BW rate
    std::vector<std::complex<double>> dn; // conjugate (down-chirp)
    std::vector<std::complex<double>> upFull; // full-rate chirp (sps samples)
    std::vector<std::complex<double>> dnFull;
};

int mod(int a, int b)
{
    const int r = a % b;
    return r < 0 ? r + b : r;
}

int grayEnc(int v) { return v ^ (v >> 1); }

// The Semtech/grelora whitening sequence (255-byte period).
const uint8_t* whiteningSeq()
{
    static const std::array<uint8_t, 255> seq = [] {
        std::array<uint8_t, 255> s{};
        uint8_t x = 0xFF;
        for (int i = 0; i < 255; ++i) {
            s[i] = x;
            const uint8_t fb = static_cast<uint8_t>(((x >> 7) ^ (x >> 5) ^ (x >> 4) ^ (x >> 3)) & 1);
            x = static_cast<uint8_t>((x << 1) | fb);
        }
        return s;
    }();
    return seq.data();
}

uint16_t crc16Bytes(const uint8_t* data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        for (int k = 0; k < 8; ++k) {
            if (((crc & 0x8000) >> 8) ^ (b & 0x80))
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            else
                crc = static_cast<uint16_t>(crc << 1);
            b = static_cast<uint8_t>(b << 1);
        }
    }
    return crc;
}

void buildChirps(Ctx& c)
{
    const int N = c.p.N;
    c.up.resize(N);
    c.dn.resize(N);
    for (int n = 0; n < N; ++n) {
        const double phase = 2.0 * PI * (static_cast<double>(n) * n / (2.0 * N) - 0.5 * n);
        c.up[n] = std::polar(1.0, phase);
        c.dn[n] = std::conj(c.up[n]);
    }

    const int sps = c.p.sps;
    const double os2 = static_cast<double>(c.p.os) * c.p.os;
    c.upFull.resize(sps);
    c.dnFull.resize(sps);
    for (int n = 0; n < sps; ++n) {
        const double phase = 2.0 * PI *
            (static_cast<double>(n) * n / (2.0 * N * os2) -
             0.5 * static_cast<double>(n) / c.p.os);
        c.upFull[static_cast<size_t>(n)] = std::polar(1.0, phase);
        c.dnFull[static_cast<size_t>(n)] = std::conj(c.upFull[static_cast<size_t>(n)]);
    }
}

std::vector<std::complex<double>> decimate(const std::vector<std::complex<double>>& iq,
                                           int start, int maxOut, const LoraParams& p)
{
    std::vector<std::complex<double>> d;
    d.reserve(maxOut);
    const int base = start + p.os / 2;
    for (int k = 0; k < maxOut; ++k) {
        const long long idx = static_cast<long long>(base) + static_cast<long long>(p.os) * k;
        if (idx < 0 || idx >= static_cast<long long>(iq.size())) break;
        d.push_back(iq[static_cast<size_t>(idx)]);
    }
    return d;
}

// Preamble detector: the same up-chirp repeated N times concentrates all
// dechirped energy into one FFT bin.  Data symbols do not.
double preambleScore(const std::vector<std::complex<double>>& iq,
                     int start, int nSym, const Ctx& c)
{
    const int N = c.p.N;
    std::vector<std::complex<double>> tmp(N);
    int refBin = -1;
    double minPeak = std::numeric_limits<double>::max();
    double totalSum = 0.0;

    for (int j = 0; j < nSym; ++j) {
        const long long base = static_cast<long long>(start) + c.p.os / 2 +
                               static_cast<long long>(c.p.os) * j * N;
        if (base + static_cast<long long>(c.p.os) * (N - 1) >= static_cast<long long>(iq.size()))
            return -1.0;
        for (int n = 0; n < N; ++n)
            tmp[static_cast<size_t>(n)] =
                iq[static_cast<size_t>(base + static_cast<long long>(c.p.os) * n)] *
                c.dn[static_cast<size_t>(n)];
        fft(tmp);

        int pk = 0;
        double pmax = -1.0, tsum = 0.0;
        for (int k = 0; k < N; ++k) {
            const double e = std::norm(tmp[static_cast<size_t>(k)]);
            tsum += e;
            if (e > pmax) { pmax = e; pk = k; }
        }
        if (j == 0) {
            refBin = pk;
        } else {
            const int rawDiff = std::abs(pk - refBin);
            const int binDiff = std::min(rawDiff, N - rawDiff);
            if (binDiff > 1) return 0.0; // not the repeated preamble chirp
        }
        minPeak = std::min(minPeak, pmax);
        totalSum += tsum;
    }

    const double avgTotal = totalSum / nSym;
    return avgTotal > 0.0 ? minPeak / avgTotal : 0.0;
}

int findPreambleInRange(const std::vector<std::complex<double>>& iq, int lo, int hi,
                        const LoraParams& p, const Ctx& c, double& score)
{
    const int last = std::min(static_cast<int>(iq.size()) - 12 * p.sps, hi);
    lo = std::max(0, lo);
    if (last < lo) return -1;

    // Coarse scan (half a symbol step) over the whole burst, then refine.
    const int step = std::max(1, p.sps / 2);
    int best = -1;
    double bestScore = -1.0;
    for (int s = lo; s <= last; s += step) {
        const double sc = preambleScore(iq, s, 8, c);
        if (sc > bestScore) { bestScore = sc; best = s; }
    }
    if (best < 0) return -1;

    const int rlo = std::max(lo, best - step);
    const int rhi = std::min(last, best + step);
    for (int s = rlo; s <= rhi; ++s) {
        const double sc = preambleScore(iq, s, 12, c);
        if (sc > bestScore) { bestScore = sc; best = s; }
    }
    score = bestScore;
    return best;
}

int circularBinDiff(int a, int b, int n)
{
    const int d = std::abs(a - b);
    return std::min(d, n - d);
}

bool symbolPeakAt(const std::vector<std::complex<double>>& iq, int start,
                  const LoraParams& p, const Ctx& c, int& bin, double& mag)
{
    const long long base = static_cast<long long>(start) + p.os / 2;
    if (base < 0 || base + static_cast<long long>(p.os) * (p.N - 1) >=
                        static_cast<long long>(iq.size()))
        return false;

    std::vector<std::complex<double>> tmp(static_cast<size_t>(p.N));
    for (int n = 0; n < p.N; ++n)
        tmp[static_cast<size_t>(n)] =
            iq[static_cast<size_t>(base + static_cast<long long>(p.os) * n)] *
            c.dn[static_cast<size_t>(n)];
    fft(tmp);
    bin = 0;
    mag = -1.0;
    for (int k = 0; k < p.N; ++k) {
        const double e = std::norm(tmp[static_cast<size_t>(k)]);
        if (e > mag) { mag = e; bin = k; }
    }
    return true;
}

// The coarse search can land part-way through the preamble.  Walk backwards
// while the dechirped peak stays on the same bin; this recovers the true
// preamble start and gives the CFO estimator a clean run of symbols.
int extendPreambleStart(const std::vector<std::complex<double>>& iq, int start,
                        const LoraParams& p, const Ctx& c)
{
    int refBin = 0;
    double refMag = 0.0;
    if (!symbolPeakAt(iq, start, p, c, refBin, refMag) || refMag <= 0.0)
        return start;

    int best = start;
    for (int k = 1; k <= 48; ++k) {
        const int s = start - k * p.sps;
        if (s < 0) break;
        int bin = 0;
        double mag = 0.0;
        if (!symbolPeakAt(iq, s, p, c, bin, mag)) break;
        if (circularBinDiff(bin, refBin, p.N) > 1) break;
        if (mag < refMag * 0.25) break;
        best = s;
    }
    return best;
}

double estimateCfo(const std::vector<std::complex<double>>& d, const LoraParams& p, const Ctx& c)
{
    constexpr int Z = 8;
    const int M = p.N * Z;
    std::vector<double> estimates;
    std::vector<int> bins;
    const int nsym = std::min(32, static_cast<int>(d.size() / p.N));
    estimates.reserve(static_cast<size_t>(nsym));
    bins.reserve(static_cast<size_t>(nsym));

    for (int j = 0; j < nsym; ++j) {
        std::vector<std::complex<double>> tmp(static_cast<size_t>(M),
                                              std::complex<double>(0.0, 0.0));
        for (int n = 0; n < p.N; ++n)
            tmp[static_cast<size_t>(n)] = d[static_cast<size_t>(j * p.N + n)] * c.dn[static_cast<size_t>(n)];
        fft(tmp);
        int idx = 0;
        double best = -1.0;
        for (int i = 0; i < M; ++i) {
            const double e = std::norm(tmp[static_cast<size_t>(i)]);
            if (e > best) { best = e; idx = i; }
        }
        const double y0 = std::abs(tmp[static_cast<size_t>((idx - 1 + M) % M)]);
        const double y1 = std::abs(tmp[static_cast<size_t>(idx)]);
        const double y2 = std::abs(tmp[static_cast<size_t>((idx + 1) % M)]);
        const double den = y0 - 2.0 * y1 + y2;
        const double frac = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        const double peakBin = (idx + frac) / Z;
        estimates.push_back(peakBin - 1.0);
        bins.push_back(static_cast<int>(std::llround(peakBin)));
    }
    if (estimates.empty()) return 0.0;

    // Keep only the leading run of symbols that share the preamble peak bin.
    int run = 1;
    while (run < static_cast<int>(bins.size()) &&
           circularBinDiff(bins[static_cast<size_t>(run)], bins[0], M / Z) <= 1)
        ++run;
    if (run < 4) run = std::min<int>(16, static_cast<int>(estimates.size()));
    run = std::min(run, 24);

    std::vector<double> vals(estimates.begin(), estimates.begin() + run);
    std::sort(vals.begin(), vals.end());
    return vals[vals.size() / 2];
}

void applyCfo(std::vector<std::complex<double>>& d, double cfoBins, const LoraParams& p)
{
    for (size_t k = 0; k < d.size(); ++k) {
        const double phase = -2.0 * PI * cfoBins * static_cast<double>(k) / p.N;
        d[k] *= std::polar(1.0, phase);
    }
}

// Estimate the true (unaliased) CFO at the full sample rate.  This can be
// tens of kHz away from the tuned centre; the decimated domain would fold it.
double estimateCfoFull(const std::vector<std::complex<double>>& iq, int start,
                       const LoraParams& p, const Ctx& c)
{
    const int sps = p.sps;
    constexpr int Z = 4;
    const int M = sps * Z;
    std::vector<double> est;
    const int nsym = std::min(16, static_cast<int>((static_cast<int>(iq.size()) - start) / sps));
    est.reserve(static_cast<size_t>(std::max(0, nsym)));

    for (int j = 0; j < nsym; ++j) {
        const long long base = static_cast<long long>(start) + static_cast<long long>(j) * sps;
        if (base + sps > static_cast<long long>(iq.size())) break;
        std::vector<std::complex<double>> tmp(static_cast<size_t>(M), std::complex<double>(0.0, 0.0));
        for (int n = 0; n < sps; ++n)
            tmp[static_cast<size_t>(n)] = iq[static_cast<size_t>(base + n)] * c.dnFull[static_cast<size_t>(n)];
        fft(tmp);

        int idx = 0;
        double best = -1.0;
        for (int i = 0; i < M; ++i) {
            const double e = std::norm(tmp[static_cast<size_t>(i)]);
            if (e > best) { best = e; idx = i; }
        }
        const double y0 = std::abs(tmp[static_cast<size_t>((idx - 1 + M) % M)]);
        const double y1 = std::abs(tmp[static_cast<size_t>(idx)]);
        const double y2 = std::abs(tmp[static_cast<size_t>((idx + 1) % M)]);
        const double den = y0 - 2.0 * y1 + y2;
        const double frac = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        double peakBin = idx + frac;
        if (peakBin > M / 2.0) peakBin -= M;
        est.push_back(peakBin / Z);
    }

    if (est.empty()) return 0.0;
    std::sort(est.begin(), est.end());
    return est[est.size() / 2] - 1.0; // nominal preamble bin is 1
}

int symbolRaw(const std::vector<std::complex<double>>& d, int pos,
              const LoraParams& p, const Ctx& c, double* quality = nullptr,
              bool full = false)
{
    const int len = full ? p.sps : p.N;
    const auto& ref = full ? c.dnFull : c.dn;
    std::vector<std::complex<double>> tmp(static_cast<size_t>(len));
    for (int n = 0; n < len; ++n)
        tmp[static_cast<size_t>(n)] = d[static_cast<size_t>(pos + n)] * ref[static_cast<size_t>(n)];
    fft(tmp);
    int k = 0;
    double best = -1.0, sum = 0.0;
    for (int i = 0; i < len; ++i) {
        const double e = std::norm(tmp[static_cast<size_t>(i)]);
        sum += e;
        if (e > best) { best = e; k = i; }
    }
    if (quality) *quality = sum > 0.0 ? best / sum : 0.0;
    return k;
}

std::vector<int> deinterleave(const std::vector<int>& syms, int sfApp, int cwLen)
{
    std::vector<std::vector<int>> de(sfApp, std::vector<int>(cwLen, 0));
    for (int i = 0; i < cwLen; ++i) {
        const int v = syms[static_cast<size_t>(i)];
        for (int j = 0; j < sfApp; ++j) {
            const int bit = (v >> (sfApp - 1 - j)) & 1;
            de[static_cast<size_t>(mod(i - j - 1, sfApp))][static_cast<size_t>(i)] = bit;
        }
    }
    std::vector<int> out(sfApp, 0);
    for (int r = 0; r < sfApp; ++r) {
        int v = 0;
        for (int i = 0; i < cwLen; ++i) v = (v << 1) | de[static_cast<size_t>(r)][static_cast<size_t>(i)];
        out[static_cast<size_t>(r)] = v;
    }
    return out;
}

int hammingDecode(int cw, int crApp)
{
    const int cwLen = crApp + 4;
    int c[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int j = 0; j < cwLen; ++j) c[j] = (cw >> (cwLen - 1 - j)) & 1;

    int d[4] = {c[3], c[2], c[1], c[0]};

    bool correct = false;
    if (crApp == 4) {
        int ones = 0;
        for (int j = 0; j < cwLen; ++j) ones += c[j];
        correct = (ones & 1) != 0;
    } else if (crApp == 3) {
        correct = true;
    }

    if (correct) {
        const int s0 = c[0] ^ c[1] ^ c[2] ^ c[4];
        const int s1 = c[1] ^ c[2] ^ c[3] ^ c[5];
        const int s2 = c[0] ^ c[1] ^ c[3] ^ c[6];
        const int synd = s0 | (s1 << 1) | (s2 << 2);
        switch (synd) {
            case 5: d[3] ^= 1; break;
            case 7: d[2] ^= 1; break;
            case 3: d[1] ^= 1; break;
            case 6: d[0] ^= 1; break;
            default: break;
        }
    }

    return (d[0] << 3) | (d[1] << 2) | (d[2] << 1) | d[3];
}

// Soft-decision helpers -----------------------------------------------------
struct HamEntry {
    uint8_t cw;
    uint8_t nibble;
};

std::array<HamEntry, 16> makeHamTable(int crApp)
{
    std::array<HamEntry, 16> t{};
    for (int n = 0; n < 16; ++n) {
        const int db[4] = {(n >> 3) & 1, (n >> 2) & 1, (n >> 1) & 1, n & 1};
        int cw = 0;
        if (crApp != 1) {
            const int p0 = db[3] ^ db[2] ^ db[1];
            const int p1 = db[2] ^ db[1] ^ db[0];
            const int p2 = db[3] ^ db[2] ^ db[0];
            const int p3 = db[3] ^ db[1] ^ db[0];
            cw = (db[3] << 7) | (db[2] << 6) | (db[1] << 5) | (db[0] << 4) |
                 (p0 << 3) | (p1 << 2) | (p2 << 1) | p3;
            cw >>= (4 - crApp);
        } else {
            const int p4 = db[0] ^ db[1] ^ db[2] ^ db[3];
            cw = (db[3] << 4) | (db[2] << 3) | (db[1] << 2) | (db[0] << 1) | p4;
        }
        t[static_cast<size_t>(n)] = {static_cast<uint8_t>(cw), static_cast<uint8_t>(n)};
    }
    return t;
}

const std::array<HamEntry, 16>& hamTable(int crApp)
{
    static const auto t1 = makeHamTable(1);
    static const auto t2 = makeHamTable(2);
    static const auto t3 = makeHamTable(3);
    static const auto t4 = makeHamTable(4);
    switch (crApp) {
        case 1: return t1;
        case 2: return t2;
        case 3: return t3;
        default: return t4;
    }
}

int softHamDecode(const std::vector<double>& llr, int crApp)
{
    const auto& table = hamTable(crApp);
    const int cwLen = crApp + 4;
    int bestIdx = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (int e = 0; e < 16; ++e) {
        double score = 0.0;
        for (int j = 0; j < cwLen; ++j) {
            const int bit = (table[static_cast<size_t>(e)].cw >> (cwLen - 1 - j)) & 1;
            score += bit ? llr[static_cast<size_t>(j)] : -llr[static_cast<size_t>(j)];
        }
        if (score > bestScore) { bestScore = score; bestIdx = e; }
    }
    return table[static_cast<size_t>(bestIdx)].nibble;
}

std::vector<std::vector<double>> deinterleaveLlr(
    const std::vector<std::vector<double>>& syms, int sfApp, int cwLen)
{
    std::vector<std::vector<double>> de(sfApp, std::vector<double>(cwLen, 0.0));
    for (int i = 0; i < cwLen; ++i) {
        for (int j = 0; j < sfApp; ++j) {
            de[static_cast<size_t>(mod(i - j - 1, sfApp))][static_cast<size_t>(i)] =
                syms[static_cast<size_t>(i)][static_cast<size_t>(j)];
        }
    }
    return de;
}

void symbolLlr(const std::vector<std::complex<double>>& d, int pos,
               const LoraParams& p, const Ctx& c,
               std::vector<double>& llr, double& quality, bool full = false)
{
    const int len = full ? p.sps : p.N;
    const auto& ref = full ? c.dnFull : c.dn;
    std::vector<std::complex<double>> tmp(static_cast<size_t>(len));
    for (int n = 0; n < len; ++n)
        tmp[static_cast<size_t>(n)] = d[static_cast<size_t>(pos + n)] * ref[static_cast<size_t>(n)];
    fft(tmp);

    double sum = 0.0, peak = 0.0;
    std::vector<double> max0(static_cast<size_t>(p.sf), 0.0);
    std::vector<double> max1(static_cast<size_t>(p.sf), 0.0);

    for (int b = 0; b < len; ++b) {
        const double mag = std::norm(tmp[static_cast<size_t>(b)]);
        sum += mag;
        peak = std::max(peak, mag);
        const int s = grayEnc(mod(b - 1, p.N));
        for (int j = 0; j < p.sf; ++j) {
            const int bit = (s >> (p.sf - 1 - j)) & 1;
            if (bit)
                max1[static_cast<size_t>(j)] = std::max(max1[static_cast<size_t>(j)], mag);
            else
                max0[static_cast<size_t>(j)] = std::max(max0[static_cast<size_t>(j)], mag);
        }
    }

    llr.resize(static_cast<size_t>(p.sf));
    for (int j = 0; j < p.sf; ++j)
        llr[static_cast<size_t>(j)] = max1[static_cast<size_t>(j)] - max0[static_cast<size_t>(j)];

    quality = sum > 0.0 ? peak / sum : 0.0;
}

int headerChecksum(const int nib[5])
{
    const int c4 = (nib[0] >> 3 & 1) ^ (nib[0] >> 2 & 1) ^ (nib[0] >> 1 & 1) ^ (nib[0] & 1);
    const int c3 = (nib[0] >> 3 & 1) ^ (nib[1] >> 3 & 1) ^ (nib[1] >> 2 & 1) ^ (nib[1] >> 1 & 1) ^ (nib[2] & 1);
    const int c2 = (nib[0] >> 2 & 1) ^ (nib[1] >> 3 & 1) ^ (nib[1] & 1) ^ (nib[2] >> 3 & 1) ^ (nib[2] >> 1 & 1);
    const int c1 = (nib[0] >> 1 & 1) ^ (nib[1] >> 2 & 1) ^ (nib[1] & 1) ^ (nib[2] >> 2 & 1) ^ (nib[2] >> 1 & 1) ^ (nib[2] & 1);
    const int c0 = (nib[0] & 1) ^ (nib[1] >> 1 & 1) ^ (nib[2] >> 3 & 1) ^ (nib[2] >> 2 & 1) ^ (nib[2] >> 1 & 1) ^ (nib[2] & 1);
    return (c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0;
}

bool decodeHeader(const std::vector<std::complex<double>>& d, int hpos,
                  const LoraParams& p, const Ctx& c, LoraHeader& h,
                  bool full = false)
{
    const int stride = full ? p.sps : p.N;
    if (hpos < 0 || hpos + 8 * stride > static_cast<int>(d.size())) return false;

    std::vector<int> syms(8);
    for (int i = 0; i < 8; ++i) {
        const int k = symbolRaw(d, hpos + i * stride, p, c, nullptr, full);
        int v = mod(k - 1, p.N);
        v /= 4; // header always uses the reduced rate (SF-2 bits/symbol)
        syms[static_cast<size_t>(i)] = grayEnc(v);
    }

    const auto cws = deinterleave(syms, p.sf - 2, 8);
    int nib[5];
    for (int i = 0; i < 5; ++i) nib[i] = hammingDecode(cws[static_cast<size_t>(i)], 4);

    const int payloadLen = (nib[0] << 4) | nib[1];
    const int crc = nib[2] & 1;
    const int cr = nib[2] >> 1;
    const int chk = ((nib[3] & 1) << 4) | nib[4];
    if (chk != headerChecksum(nib)) return false;
    if (payloadLen <= 0 || payloadLen > 184) return false;
    if (cr < 1 || cr > 4) return false;

    h.payloadLen = payloadLen;
    h.cr = cr;
    h.hasCrc = crc;
    return true;
}

bool decodePayload(const std::vector<std::complex<double>>& d, int pstart,
                   const LoraHeader& h, const LoraParams& p, const Ctx& c,
                   std::vector<uint8_t>& out, bool& crcOk, double& quality,
                   bool full = false)
{
    const int stride = full ? p.sps : p.N;
    const int cwLen = h.cr + 4;
    const int nBytes = h.payloadLen + 2 * h.hasCrc;
    const int nBlocks = static_cast<int>(std::ceil(nBytes * 8.0 / (p.sf * 4.0)));

    std::vector<int> nibbles;
    nibbles.reserve(static_cast<size_t>(nBlocks * p.sf));
    double qSum = 0.0;
    int qCount = 0;

    for (int b = 0; b < nBlocks; ++b) {
        std::vector<std::vector<double>> llrSyms(static_cast<size_t>(cwLen));
        for (int i = 0; i < cwLen; ++i) {
            const int pos = pstart + (b * cwLen + i) * stride;
            if (pos < 0 || pos + stride > static_cast<int>(d.size())) return false;
            double q = 0.0;
            symbolLlr(d, pos, p, c, llrSyms[static_cast<size_t>(i)], q, full);
            qSum += q;
            ++qCount;
        }
        const auto llrCws = deinterleaveLlr(llrSyms, p.sf, cwLen);
        for (const auto& cw : llrCws) nibbles.push_back(softHamDecode(cw, h.cr));
    }

    if (static_cast<int>(nibbles.size()) < nBytes * 2) return false;

    std::vector<uint8_t> raw(nBytes);
    for (int i = 0; i < nBytes; ++i) {
        const int lo = nibbles[static_cast<size_t>(2 * i)];
        const int hi = nibbles[static_cast<size_t>(2 * i + 1)];
        raw[static_cast<size_t>(i)] = static_cast<uint8_t>((hi << 4) | lo);
    }

    const uint8_t* seq = whiteningSeq();
    out.assign(nBytes, 0);
    for (int i = 0; i < h.payloadLen; ++i)
        out[static_cast<size_t>(i)] = static_cast<uint8_t>(raw[static_cast<size_t>(i)] ^ seq[i]);
    for (int i = h.payloadLen; i < nBytes; ++i)
        out[static_cast<size_t>(i)] = raw[static_cast<size_t>(i)]; // CRC is not whitened

    quality = qCount > 0 ? qSum / qCount : 0.0;

    crcOk = true;
    if (h.hasCrc) {
        if (h.payloadLen < 2) return false;
        uint16_t crc = crc16Bytes(out.data(), static_cast<size_t>(h.payloadLen - 2));
        crc ^= out[static_cast<size_t>(h.payloadLen - 1)];
        crc ^= static_cast<uint16_t>(out[static_cast<size_t>(h.payloadLen - 2)] << 8);
        const uint16_t got = static_cast<uint16_t>(out[static_cast<size_t>(h.payloadLen)] |
                                                   (out[static_cast<size_t>(h.payloadLen + 1)] << 8));
        crcOk = (crc == got);
    }
    return true;
}

struct Burst { int start; int end; };

std::vector<Burst> detectBursts(const std::vector<std::complex<double>>& iq, const LoraParams& p)
{
    const int win = std::max(16, p.sps / 4);
    const int nw = static_cast<int>(iq.size()) / win;
    if (nw <= 0) return {};

    std::vector<double> energy(static_cast<size_t>(nw), 0.0);
    for (int i = 0; i < nw; ++i) {
        double e = 0.0;
        const size_t base = static_cast<size_t>(i) * win;
        for (int j = 0; j < win; ++j) e += std::norm(iq[base + j]);
        energy[static_cast<size_t>(i)] = e;
    }

    std::vector<double> sorted = energy;
    const size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
    const double median = sorted[mid];
    const double peak = *std::max_element(energy.begin(), energy.end());
    // Keep this sensitive: the preamble detector and payload CRC reject noise.
    // A high threshold here was dropping weak packets before they were even
    // considered (e.g. LoRa bursts only ~10 dB above the noise floor).
    const double threshold = std::max(median * 3.0, peak * 0.001);

    std::vector<Burst> out;
    int i = 0;
    while (i < nw) {
        while (i < nw && energy[static_cast<size_t>(i)] <= threshold) ++i;
        if (i >= nw) break;
        const int first = i;
        int last = i;
        while (i < nw) {
            if (energy[static_cast<size_t>(i)] > threshold) last = i;
            else if (i - last > 8) break;
            ++i;
        }
        out.push_back({first * win, std::min(static_cast<int>(iq.size()), (last + 1) * win)});
    }
    return out;
}

bool decodeStream(const std::vector<std::complex<double>>& d, int preambleFull,
                  const LoraParams& p, const Ctx& c, LoraPacket& out,
                  std::string& log, bool verbose, const char* tag,
                  int hposLo, int hposHi, bool full)
{
    const int stride = full ? p.sps : p.N;
    const int pdMax = full ? std::max(16, p.os) : 16;

    // Try the nominal payload offset first, then increasingly large timing
    // offsets.
    std::vector<int> offsets;
    offsets.push_back(0);
    for (int dlt = 1; dlt <= pdMax; ++dlt) {
        offsets.push_back(-dlt);
        offsets.push_back(dlt);
    }

    int headersSeen = 0;
    int crcCandidates = 0;

    hposLo = std::max(0, hposLo);
    hposHi = std::min(hposHi, static_cast<int>(d.size()) - 8 * stride);
    for (int hpos = hposLo; hpos <= hposHi; ++hpos) {
        LoraHeader h;
        if (!decodeHeader(d, hpos, p, c, h, full)) continue;
        if (!h.hasCrc) continue; // MeshCore always enables the LoRa CRC

        if (verbose && headersSeen < 12) {
            std::cerr << "[lora " << tag << "] header hpos=" << hpos
                      << " PL=" << h.payloadLen << " cr=4/" << (4 + h.cr) << "\n";
            ++headersSeen;
        }

        for (int pd : offsets) {
            const int pstart = hpos + 8 * stride + pd;
            LoraPacket cand;
            cand.header = h;
            cand.preambleStart = preambleFull;
            cand.headerPos = hpos;
            cand.payloadPos = pstart;
            bool crcOk = false;
            double quality = 0.0;
            if (!decodePayload(d, pstart, h, p, c, cand.data, crcOk, quality, full))
                continue;
            if (!crcOk) continue;

            if (verbose)
                std::cerr << "[lora " << tag << "] CRC OK PL=" << h.payloadLen
                          << " pd=" << pd << "\n";
            cand.crcOk = true;
            cand.quality = quality;
            out = cand;
            log = "LoRa header checksum and payload CRC valid";
            return true;
        }
        ++crcCandidates;
    }
    if (verbose)
        std::cerr << "[lora " << tag << "] no CRC (headers tried=" << crcCandidates << ")\n";
    return false;
}

bool tryBurst(const std::vector<std::complex<double>>& iq, int burstStart, int burstEnd,
              const LoraParams& p, const Ctx& c, LoraPacket& out, std::string& log,
              bool verbose)
{
    double pscore = 0.0;
    int preamble = findPreambleInRange(iq, burstStart, burstEnd, p, c, pscore);
    if (preamble < 0 || pscore < 0.10) {
        if (verbose)
            std::cerr << "[lora] burst " << burstStart << ".." << burstEnd
                      << " no preamble (score=" << pscore << ")\n";
        return false;
    }

    preamble = extendPreambleStart(iq, preamble, p, c);

    const int maxSymbols = 400;
    const int preambleLen = (p.sf <= 8) ? 32 : 16;
    const int expectedHeader = preambleLen + 4; // +4.25 symbols; scan covers the rest
    const int narrowHalf = 4 * p.N;
    const int eighthFull = p.sps / 8;
    const int eighthDec = p.N / 8;

    auto tryDecimated = [&](int pk) -> bool {
        const int pstart = preamble + pk * eighthFull;
        if (pstart < 0) return false;
        auto d = decimate(iq, pstart, maxSymbols * p.N, p);
        if (static_cast<int>(d.size()) < 24 * p.N) return false;
        const double cfo = estimateCfo(d, p, c);
        applyCfo(d, cfo, p);
        if (verbose && pk == 0)
            std::cerr << "[lora] burst " << burstStart << ".." << burstEnd
                      << " preamble=" << preamble << " score=" << pscore
                      << " cfo=" << cfo << " bins\n";

        const int center = expectedHeader * p.N - pk * eighthDec;
        if (decodeStream(d, preamble, p, c, out, log, verbose, "dec",
                         center - narrowHalf, center + narrowHalf, false))
            return true;
        if (pk == 0 &&
            decodeStream(d, preamble, p, c, out, log, verbose, "decw",
                         0, 70 * p.N, false))
            return true;
        return false;
    };

    // Fast path: best timing phase, then full-rate demodulation.
    if (tryDecimated(0)) return true;

    // The decimated domain folds large CFOs.  Re-estimate at the full sample
    // rate (which is unambiguous across the whole captured band), shift the
    // burst down by that amount, then retry.
    const double cfoFull = estimateCfoFull(iq, preamble, p, c);
    if (verbose)
        std::cerr << "[lora] full-rate cfo=" << cfoFull << " bins ("
                  << (cfoFull * p.bw / p.N) << " Hz)\n";
    const int lo = std::max(0, preamble - p.sps);
    const int hi = std::min(static_cast<int>(iq.size()), preamble + maxSymbols * p.sps);

    if (hi - lo > 24 * p.sps) {
        std::vector<std::complex<double>> shifted(static_cast<size_t>(hi - lo));
        for (int k = 0; k < hi - lo; ++k) {
            const double phase = -2.0 * PI * cfoFull * static_cast<double>(k) / p.sps;
            shifted[static_cast<size_t>(k)] =
                iq[static_cast<size_t>(lo + k)] * std::polar(1.0, phase);
        }

        const int preambleShifted = preamble - lo;
        // Demodulate directly at the full sample rate: this avoids the
        // decimation phase ambiguity.
        const int headerCenterFull = preambleShifted + expectedHeader * p.sps;
        const int halfWin = std::max(p.N, p.sps / 2);
        if (decodeStream(shifted, preamble, p, c, out, log, verbose, "full",
                         headerCenterFull - halfWin, headerCenterFull + halfWin, true))
            return true;
    }

    // Last resort: try the remaining fractional timing phases.  This is slower
    // but recovers captures whose preamble lands on an awkward sample phase.
    static const int phaseKs[] = {1, -1, 2, -2, 3, 4, 5, 6, 7, 8};
    for (int pk : phaseKs) {
        if (tryDecimated(pk)) return true;
    }
    return false;
}

} // namespace

bool initLora(LoraParams& p, std::string& err)
{
    if (p.sf < 5 || p.sf > 12) { err = "spreading factor must be 5..12"; return false; }
    const double osd = p.fs / p.bw;
    p.os = static_cast<int>(std::llround(osd));
    if (p.os < 1 || std::fabs(osd - p.os) > 1e-6) {
        err = "sample rate must be an integer multiple of bandwidth";
        return false;
    }
    p.N = 1 << p.sf;
    p.sps = p.N * p.os;
    return true;
}

bool decodeLora(const std::vector<std::complex<double>>& iq,
                const LoraParams& p, LoraPacket& out, std::string& log, bool verbose)
{
    Ctx c{p, {}, {}, {}, {}};
    buildChirps(c);

    auto bursts = detectBursts(iq, p);
    if (bursts.empty())
        bursts.push_back({0, static_cast<int>(iq.size())}); // scan the whole capture

    for (const auto& b : bursts) {
        std::string why;
        if (tryBurst(iq, b.start, b.end, p, c, out, why, verbose)) {
            log = why;
            return true;
        }
    }
    log = "no decodable LoRa packet found";
    return false;
}

} // namespace mc
