// Full-range McLeod Pitch Method detector for the ToneMap view.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// Pure float code with no Arduino dependency (the ESP32-S3 FPU is single
// precision only). Semantics follow pitchy's PitchDetector.findPitch:
// normalised square difference function (NSDF, type II), key maxima, first key
// maximum >= K * highest (K = 0.9), parabolic refinement. Differences from
// pitchy (see docs/TONEMAP_DESIGN.md section 3): the DC offset of the input is
// removed in prepare(), the NSDF is evaluated only up to maxLag, and a lobe that
// is still open at maxLag is accepted when its peak is already behind.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mpm {

constexpr size_t kMaxWindow = 2048;
constexpr int kMaxLag = 720;  // sizing cap: the nsdf buffer needs kMaxLag + 2 floats

// Signal levels of a prepared window, in int16 units with the DC offset removed.
struct Levels {
    float mean, rms, peak;
};

struct Result {
    bool found;     // a key maximum was found
    float freq;     // Hz (sampleRate / lag)
    float clarity;  // 0..1
    float lag;      // refined lag in samples
};

// Removes the mean of x[0..n) into work[0..n) (float) and returns the levels.
// Cheap (about 60 us for 2048 samples): the caller can gate on rms and skip findPitch().
Levels prepare(const int16_t* x, size_t n, float* work);

// McLeod pitch on a prepared window. nsdf is scratch for at least maxLag + 2 floats.
// maxLag is clamped to kMaxLag and to n - 2. found == false for silence / no key maximum.
Result findPitch(const float* work, size_t n, float sampleRate, int maxLag, float* nsdf);

// Convenience for the bench: prepare + findPitch.
inline Result detect(const int16_t* x, size_t n, float fs, int maxLag, float* work, float* nsdf) {
    prepare(x, n, work);
    return findPitch(work, n, fs, maxLag, nsdf);
}

}  // namespace mpm
