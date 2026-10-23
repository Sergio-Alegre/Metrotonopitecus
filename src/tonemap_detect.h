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

// Noise gate on the RMS of the DC-free window, int16 units (None, Min, Low, Med, High). The setting (settings tmGate,
// index into this table) belongs to the ToneMap view; the classic tuner uses AutoGate (below). Low (80) was calibrated with a viola on the
// device (docs/TONEMAP_DESIGN.md section 12); Min (30) was added for playing with a practice mute (section 16).
constexpr int kGateLevels = 5;
constexpr int kGateDefault = 2;  // Low
constexpr float kGateRms[kGateLevels] = {0.0f, 30.0f, 80.0f, 200.0f, 450.0f};
constexpr const char* const kGateNames[kGateLevels] = {"None", "Min", "Low", "Med", "High"};

// Automatic noise gate of the classic tuner (the ToneMap view keeps the manual table above). It follows the noise floor of
// the room: the floor falls at once to a quieter window and rises slowly (30 s time constant) toward louder ones, and the
// gate is 2.5 x floor clamped to 20..80. A silent room (floor ~6) gives 20, so a practice mute (playing rms p5 ~27) is
// detected; a noisy room raises it up to the old fixed 80, never above. Docs: TONEMAP_DESIGN.md section 16.
constexpr float kAutoGateMul = 2.5f;
constexpr float kAutoGateMin = 20.0f;
constexpr float kAutoGateMax = 80.0f;
constexpr float kAutoGateTauMs = 30000.0f;
constexpr float kAutoGateMinFloor = 1.0f;
constexpr uint32_t kAutoGateDefaultDtMs = 32;  // used when a window reports dt = 0

struct AutoGate {
    void reset() {
        floor_ = 0.0f;
        init_ = false;
    }

    // Feed the rms of EVERY analysed window (also those below the gate, so that the floor can fall in silence) and use
    // the returned threshold for THIS window. A NaN or negative rms carries no information: the floor is left alone
    // (before the first valid window the highest gate, 80, is returned). rms is capped at 32768 (int16 full scale).
    float update(float rms, uint32_t dtMs) {
        if (rms >= 0.0f) {  // false for NaN
            if (rms > 32768.0f) rms = 32768.0f;
            if (!init_) {
                floor_ = rms > kAutoGateMinFloor ? rms : kAutoGateMinFloor;
                init_ = true;
            } else if (rms < floor_) {
                floor_ = rms > kAutoGateMinFloor ? rms : kAutoGateMinFloor;
            } else {
                const float dt = (float)(dtMs == 0 ? kAutoGateDefaultDtMs : dtMs);
                const float k = dt >= kAutoGateTauMs ? 1.0f : dt / kAutoGateTauMs;
                floor_ += (rms - floor_) * k;
            }
        }
        if (!init_) return kAutoGateMax;
        const float g = kAutoGateMul * floor_;
        return g < kAutoGateMin ? kAutoGateMin : (g > kAutoGateMax ? kAutoGateMax : g);
    }

    float floor() const { return floor_; }

private:
    float floor_ = 0.0f;  // noise-floor estimate, rms units
    bool init_ = false;
};

// Gate index from a setting value, kept inside the table.
inline int gateIndex(int setting) { return setting < 0 ? 0 : (setting >= kGateLevels ? kGateLevels - 1 : setting); }
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
    float gate;      // the gate threshold this window was compared with
};

// The window buffer (tonemap_audio::kWindow samples): destination of tonemap_audio::takeWindow().
int16_t* window();

// Analyses window() with the given gate (an RMS in int16 units, see kGateRms).
Analysis analyse(float gateRms);

// Same, with the automatic gate: mpm::prepare runs for every window, the gate is updated with the window rms and then
// the window is compared with the threshold it returns (findPitch is skipped below it). dtMs = WindowInfo.dtMs.
Analysis analyse(AutoGate& gate, uint32_t dtMs);

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
