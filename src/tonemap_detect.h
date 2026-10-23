// Analysis of one capture window, shared by the ToneMap view and the classic tuner.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// One 2048-sample window at 32 kHz (tonemap_audio::takeWindow) goes through mpm::prepare, the
// noise gate on its RMS, mpm::findPitch (hybrid-K detector, maxLag 720) and the validity gates
// (clarity, 14 < Hz < 8200). The three scratch buffers (about 15 KB of DRAM) live here so that both
// views share them; only one view runs at a time. See docs/TONEMAP_DESIGN.md sections 3 and 12.
#pragma once
#include <math.h>
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

// ---- octave-continuity guard -----------------------------------------------------------------------------
// The hybrid-K detector still produces short one-octave excursions on low viola notes (F3/E3/D3 read as
// F4/E4/D4 for 1-6 frames, a few A3 -> A2 frames from the K = 0.97 re-pick). A frame whose frequency is about exactly
// one octave above or below the previous raw frame is therefore reported folded back into the previous octave,
// and only accepted as a real octave jump when it persists for kOctaveGuardFrames consecutive frames. Anything else
// (glide, vibrato, another note) is accepted at once and breaks the count. Docs: TONEMAP_DESIGN.md section 15.
constexpr int kOctaveGuardFrames = 4;       // consecutive octave-off frames needed to accept the jump (~128 ms)
constexpr float kOctaveGuardTolOct = 0.04f; // "about one octave": |log2(ratio) -+ 1| < 0.04 octave (48 cents)

// One instance per consumer (each view keeps its own). Call filter() once per analysed window, after the validity gates,
// with the raw (untransposed) frequency; use the returned frequency instead of hz. An invalid window resets the guard
// (consistent with the ToneMap model, where an invalid frame ends the note); consumers also call reset() wherever
// they drop the active note, so the guard never outlives it.
struct OctaveGuard {
    void reset() {
        ref_ = 0.0f;
        count_ = 0;
    }

    float filter(float hz, bool valid) {
        if (!valid || !(hz > 0.0f)) {
            reset();
            return hz;
        }
        if (!(ref_ > 0.0f)) {  // first valid frame after a reset
            ref_ = hz;
            count_ = 0;
            return hz;
        }
        const float l = log2f(hz / ref_);
        const bool up = fabsf(l - 1.0f) < kOctaveGuardTolOct;
        const bool down = fabsf(l + 1.0f) < kOctaveGuardTolOct;
        if (up || down) {
            if (++count_ >= kOctaveGuardFrames) {  // it stays: a real octave jump, follow it
                ref_ = hz;
                count_ = 0;
                return hz;
            }
            return up ? hz * 0.5f : hz * 2.0f;  // folded; ref_ is kept, so the note stays in the previous octave
        }
        ref_ = hz;
        count_ = 0;
        return hz;
    }

private:
    float ref_ = 0.0f;  // last accepted raw frequency (0 = none)
    int count_ = 0;     // consecutive octave-off frames since ref_
};

}  // namespace tonemap_detect
