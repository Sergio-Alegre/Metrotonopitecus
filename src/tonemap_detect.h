// Analysis of one capture window, shared by the ToneMap view and the classic tuner.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// One 2048-sample window at 32 kHz (tonemap_audio::takeWindow) goes through mpm::prepare, the
// noise gate on its RMS, mpm::findPitch (hybrid-K detector, maxLag 720) and the validity gates
// (clarity, 14 < Hz < 8200). The three scratch buffers (about 15 KB of DRAM) live here so that both
// views share them; only one view runs at a time. See docs/TONEMAP_DESIGN.md sections 3 and 12.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tonemap_detect {

// Noise gate on the RMS of the DC-free window, int16 units (None, Low, Med, High).
// Calibrated with a viola on the device (docs/TONEMAP_DESIGN.md section 12).
constexpr float kGateRms[4] = {0.0f, 80.0f, 200.0f, 450.0f};
constexpr float kMinClarity = 0.8f;
constexpr float kMinHz = 14.0f;  // tonemap.live: 14 < pitch < 8200 on the untransposed pitch
constexpr float kMaxHz = 8200.0f;
constexpr int kMaxLag = 720;     // 44.4 Hz at 32 kHz (docs/TONEMAP_DESIGN.md section 0.3)

struct Analysis {
    bool loud;       // rms > 0 and rms >= gate (findPitch ran)
    bool valid;      // loud, a key maximum was found and the clarity / frequency gates passed
    float hz;        // detected frequency; 0 unless valid
    float rms;       // int16 units, DC removed
    float peak;      // int16 units, DC removed
    float clarity;   // 0 when findPitch did not run or found nothing
};

// The window buffer (tonemap_audio::kWindow samples): destination of tonemap_audio::takeWindow().
int16_t* window();

// Analyses window() with the given gate (an RMS in int16 units, see kGateRms).
Analysis analyse(float gateRms);

}  // namespace tonemap_detect
