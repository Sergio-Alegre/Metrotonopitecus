// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
#include "tonemap_detect.h"

#include "pitch_mpm.h"
#include "tonemap_audio.h"

namespace tonemap_detect {

namespace {

// Analysis buffers (DRAM, static).
int16_t winBuf[tonemap_audio::kWindow];
float workBuf[tonemap_audio::kWindow];
float nsdfBuf[mpm::kMaxLag + 2];

}  // namespace

int16_t* window() { return winBuf; }

Analysis analyse(float gateRms) {
    Analysis a = {};
    const mpm::Levels lv = mpm::prepare(winBuf, tonemap_audio::kWindow, workBuf);
    a.rms = lv.rms;
    a.peak = lv.peak;
    // Energy check (rms > 0) plus the noise gate; a gated window skips the expensive search.
    a.loud = lv.rms > 0.0f && lv.rms >= gateRms;
    if (!a.loud) return a;

    const mpm::Result r =
        mpm::findPitch(workBuf, tonemap_audio::kWindow, tonemap_audio::effectiveRate(), kMaxLag, nsdfBuf);
    if (r.found) {
        a.clarity = r.clarity;
        // Comparisons are false for NaN, so a NaN result is never valid.
        a.valid = r.clarity > kMinClarity && r.freq > kMinHz && r.freq < kMaxHz;
        if (a.valid) a.hz = r.freq;
    }
    return a;
}

}  // namespace tonemap_detect
