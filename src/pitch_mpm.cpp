// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// Arduino-ESP32 builds with -Os; this file is the CPU hot spot of the ToneMap
// view, so it alone is compiled with -O3.
#pragma GCC optimize("O3")

#include "pitch_mpm.h"

#include <math.h>

namespace mpm {

namespace {
constexpr float kClarityThreshold = 0.9f;  // pitchy's K
}

Levels prepare(const int16_t* x, size_t n, float* work) {
    Levels lv = {0.0f, 0.0f, 0.0f};
    if (n == 0) return lv;

    int32_t sum = 0;  // |sum| <= 2048 * 32768, fits
    for (size_t i = 0; i < n; ++i) sum += x[i];
    const float mean = (float)sum / (float)n;

    float sq = 0.0f, peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float v = (float)x[i] - mean;
        work[i] = v;
        sq += v * v;
        const float a = fabsf(v);
        if (a > peak) peak = a;
    }
    lv.mean = mean;
    lv.rms = sqrtf(sq / (float)n);
    lv.peak = peak;
    return lv;
}

Result findPitch(const float* w, size_t nSize, float sampleRate, int maxLag, float* nsdf) {
    Result res = {false, 0.0f, 0.0f, 0.0f};
    if (w == nullptr || nsdf == nullptr || nSize < 8 || !(sampleRate > 0.0f)) return res;
    const int n = (int)nSize;
    if (maxLag > kMaxLag) maxLag = kMaxLag;
    if (maxLag > n - 2) maxLag = n - 2;
    if (maxLag < 4) return res;

    // m'(0) = 2 * sum(w^2). Silence (or NaN input) has no pitch.
    float energy = 0.0f;
    for (int i = 0; i < n; ++i) energy += w[i] * w[i];
    if (!(energy > 0.0f)) return res;
    float m = 2.0f * energy;

    // ---- NSDF, type II: n(tau) = 2 r'(tau) / m'(tau), tau = 0..maxLag ----
    // r'(tau) = sum_{j=0}^{n-1-tau} w[j] w[j+tau] by direct summation, four lags at
    // once so every loaded sample feeds several multiply-adds. m'(tau+1) =
    // m'(tau) - w[tau]^2 - w[n-1-tau]^2, exactly as pitchy; m' never increases, so
    // once it is <= 0 the rest of the NSDF is 0.
    nsdf[0] = 1.0f;
    int t = 1;
    for (; t + 3 <= maxLag; t += 4) {
        const float* a = w;
        const float* b = w + t;
        const int len = n - t - 3;  // common range j in [0, len) for the four lags
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int j = 0;
        for (; j + 2 <= len; j += 2) {
            const float a0 = a[j], a1 = a[j + 1];
            const float b0 = b[j], b1 = b[j + 1], b2 = b[j + 2], b3 = b[j + 3], b4 = b[j + 4];
            s0 += a0 * b0;
            s1 += a0 * b1;
            s2 += a0 * b2;
            s3 += a0 * b3;
            s0 += a1 * b1;
            s1 += a1 * b2;
            s2 += a1 * b3;
            s3 += a1 * b4;
        }
        if (j < len) {
            const float a0 = a[j];
            s0 += a0 * b[j];
            s1 += a0 * b[j + 1];
            s2 += a0 * b[j + 2];
            s3 += a0 * b[j + 3];
        }
        // Lag t+k also covers j in [len, n-t-k): 3, 2, 1 and 0 extra terms.
        for (int jj = len; jj < n - t; ++jj) s0 += a[jj] * b[jj];
        for (int jj = len; jj < n - t - 1; ++jj) s1 += a[jj] * b[jj + 1];
        for (int jj = len; jj < n - t - 2; ++jj) s2 += a[jj] * b[jj + 2];

        const float s[4] = {s0, s1, s2, s3};
        for (int k = 0; k < 4; ++k) {
            const int tau = t + k;
            m -= w[tau - 1] * w[tau - 1] + w[n - tau] * w[n - tau];
            nsdf[tau] = (m > 0.0f) ? (2.0f * s[k]) / m : 0.0f;
        }
    }
    for (; t <= maxLag; ++t) {  // up to three remaining lags
        float s = 0.0f;
        const int len = n - t;
        for (int j = 0; j < len; ++j) s += w[j] * w[j + t];
        m -= w[t - 1] * w[t - 1] + w[n - t] * w[n - t];
        nsdf[t] = (m > 0.0f) ? (2.0f * s) / m : 0.0f;
    }

    // ---- Key maxima (pitchy's getKeyMaximumIndices) ----
    // A key maximum is the highest local maximum between a positively sloped zero
    // crossing and the next negatively sloped one. nsdf[0] = 1 never counts as a
    // crossing, so the initial lobe around lag 0 is skipped.
    uint16_t keys[kMaxLag / 2 + 2];
    int keyCount = 0;
    bool looking = false;
    float mx = 0.0f;
    int maxIndex = -1;
    for (int i = 1; i <= maxLag; ++i) {
        const float prev = nsdf[i - 1], cur = nsdf[i];
        if (prev <= 0.0f && cur > 0.0f) {  // positively sloped zero crossing
            looking = true;
            maxIndex = i;
            mx = cur;
        } else if (prev > 0.0f && cur <= 0.0f) {  // negatively sloped zero crossing
            looking = false;
            if (maxIndex != -1 && keyCount < (int)(sizeof(keys) / sizeof(keys[0]))) keys[keyCount++] = (uint16_t)maxIndex;
        } else if (looking && cur > mx) {
            mx = cur;
            maxIndex = i;
        }
    }
    // Intentional difference from pitchy: a lobe still open at maxLag counts when its
    // peak is already behind (maxIndex <= maxLag-2 and the next sample is not higher).
    // A lobe whose peak was not reached yet is discarded.
    if (looking && maxIndex != -1 && maxIndex <= maxLag - 2 && nsdf[maxIndex + 1] <= nsdf[maxIndex] &&
        keyCount < (int)(sizeof(keys) / sizeof(keys[0]))) {
        keys[keyCount++] = (uint16_t)maxIndex;
    }
    if (keyCount == 0) return res;

    // ---- First key maximum >= K * highest key maximum ----
    float nMax = nsdf[keys[0]];
    for (int k = 1; k < keyCount; ++k) {
        if (nsdf[keys[k]] > nMax) nMax = nsdf[keys[k]];
    }
    const float threshold = kClarityThreshold * nMax;
    int idx = keys[0];
    for (int k = 0; k < keyCount; ++k) {
        if (nsdf[keys[k]] >= threshold) {
            idx = keys[k];
            break;
        }
    }

    // ---- Parabolic refinement (shifted so the vertex offset stays small and float-safe) ----
    // idx is in [1, maxLag-1], so both neighbours exist.
    const float ya = nsdf[idx - 1], yb = nsdf[idx], yc = nsdf[idx + 1];
    const float den = ya - 2.0f * yb + yc;
    const float delta = (den < 0.0f) ? 0.5f * (ya - yc) / den : 0.0f;
    float clarity = yb - 0.25f * (ya - yc) * delta;
    if (clarity > 1.0f) clarity = 1.0f;
    const float lag = (float)idx + delta;
    if (!(lag > 0.0f)) return res;

    res.found = true;
    res.lag = lag;
    res.freq = sampleRate / lag;
    res.clarity = clarity;
    return res;
}

}  // namespace mpm
