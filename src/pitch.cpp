#include "pitch.h"

#include <math.h>

// Note: the ESP32-S3 FPU is single precision only; doubles would be emulated
// in software and far too slow here, so everything accumulates in float.

namespace pitch {

namespace {
constexpr int kMaxLag = 512;
}

Result detect(const int16_t* samples, size_t n, float sampleRate, float targetHz, float spanSemitones, float* work) {
    Result r;
    if (n < 64) return r;

    int32_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += samples[i];
    const float mean = (float)sum / (float)n;

    float sq = 0;
    for (size_t i = 0; i < n; ++i) {
        work[i] = (float)samples[i] - mean;
        sq += work[i] * work[i];
    }
    r.rms = sqrtf(sq / (float)n);

    const float ratio = powf(2.0f, spanSemitones / 12.0f);
    int tMin = (int)floorf(sampleRate / (targetHz * ratio)) - 1;
    if (tMin < 2) tMin = 2;
    const int tMax = (int)ceilf(sampleRate / (targetHz / ratio)) + 1;
    if (tMax + 2 >= kMaxLag) return r;
    const int w = (int)n - tMax - 2;
    if (w < 2 * tMax) return r;

    float e0 = 0;
    for (int j = 0; j < w; ++j) e0 += work[j] * work[j];
    if (e0 <= 0) return r;

    // nsdf[t - base] for t in [tMin - 1, tMax + 1]
    static float nsdf[kMaxLag];
    const int base = tMin - 1;
    for (int t = tMin - 1; t <= tMax + 1; ++t) {
        float corr = 0, eb = 0;
        const float* b = work + t;
        for (int j = 0; j < w; ++j) {
            corr += work[j] * b[j];
            eb += b[j] * b[j];
        }
        const float m = e0 + eb;
        nsdf[t - base] = m > 0 ? 2.0f * corr / m : 0.0f;
    }

    int bestT = -1;
    float bestV = -2.0f;
    for (int t = tMin; t <= tMax; ++t) {
        const float v = nsdf[t - base];
        if (v > nsdf[t - 1 - base] && v >= nsdf[t + 1 - base] && v > bestV) {
            bestT = t;
            bestV = v;
        }
    }
    if (bestT < 0) return r;

    const float ya = nsdf[bestT - 1 - base];
    const float yb = nsdf[bestT - base];
    const float yc = nsdf[bestT + 1 - base];
    const float den = ya - 2.0f * yb + yc;
    const float delta = den != 0.0f ? 0.5f * (ya - yc) / den : 0.0f;

    r.valid = true;
    r.freq = sampleRate / ((float)bestT + delta);
    r.clarity = yb - 0.25f * (ya - yc) * delta;
    return r;
}

}  // namespace pitch
