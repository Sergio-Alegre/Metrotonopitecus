// ToneMap heat-map model: per-note intonation history, colours, fade, re-basing.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// Pure C++11 float code with no Arduino dependency: the clock is passed in as `nowMs`
// (uint32 milliseconds, monotonic; the view stops it while paused). The semantics follow
// tonemap.live (index.html: tick, freqToNoteData, getTargetOffsetCents, bias*,
// finalizeActiveSegment, getRecentWeightedErr, rebaseErrRatio, updateCellDisplayColor,
// recomputeAllCellColorsAndRender, shiftCellStateBySemitones) as corrected by
// docs/TONEMAP_BRIEF.md section 3.6, with the ESP32 adaptation of docs/TONEMAP_DESIGN.md
// sections 4.3, 5 and 9 (chunked history, errors stored relative to A=440, lazy fade).
//
// State is indexed by CONCERT midi (0..127). The grid shows WRITTEN midi = concert - transposition.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tonemap {

constexpr int kNoteCount = 128;              // indexed by CONCERT midi, 0..127
constexpr int kGridLo = 36, kGridHi = 107;   // WRITTEN midi shown by the view (C2..B7)

enum class Sensitivity : uint8_t { Relaxed, Medium, Hard };

struct SensConst {
    float deadzone;     // |errRatio| <= deadzone: "in tune" (needle / LED) and pure-centre colour
    float errScale;     // colour uses errRatio * errScale
    float spanMul;      // colour saturates at deadzone * (1 + spanMul) (device feedback 2026-10-22: 2 / 2 / 4)
    float needleTauMs;  // needle EMA time constant (only Relaxed smooths; see tick())
};
// Relaxed {0.0025,0.7,2,350}, Medium {0.0021,0.7,2,80}, Hard {0.00105,0.9,4,30}.
// tonemap.live uses spanMul 4 / 4 / 8; the narrower ramp is a deliberate deviation (bolder map, see
// docs/TONEMAP_DESIGN.md section 13): full colour at +-18.5 c (Relaxed), +-15.5 c (Medium), +-10.1 c (Hard).
extern const SensConst kSens[3];

constexpr uint32_t kHistoryMs[4] = {1000, 3000, 30000, 0};                // 0 = unlimited
constexpr int32_t kHoldMs[6] = {5000, 15000, 30000, 120000, 300000, -1};  // -1 = no fade
constexpr int8_t kTransposeSemis[4] = {0, -2, 3, 5};

// ToneMap constants. Two of them deliberately differ from tonemap.live to make the map more visible at a
// glance (device feedback 2026-10-22, docs/TONEMAP_DESIGN.md section 13): FULL_CONF_MS is 250 (ToneMap: 500)
// and STAIN_MIN_FRACTION is 0.45 (ToneMap: 0.25).
constexpr float kFullConfMs = 250.0f;     // FULL_CONF_MS (ToneMap: 500)
constexpr float kStainMinFraction = 0.45f;  // STAIN_MIN_FRACTION (ToneMap: 0.25)
constexpr uint32_t kFadeMs = 10000;       // STAIN_FADE_SECONDS
constexpr float kFadeInMs = 125.0f;       // STAIN_FADE_IN_MS (Relaxed display EMA)

// Storage geometry (docs/TONEMAP_DESIGN.md section 5.3).
constexpr int kFineCap = 34;              // fine chunks (>= 96 ms each): >= 3.26 s
constexpr int kCoarseCap = 29;            // coarse chunks (>= 1000 ms each): ~30.6 s
constexpr uint32_t kFineChunkMs = 96;
constexpr uint32_t kCoarseChunkMs = 1000;
// One frame never contributes more than this (guards the 16/32-bit chunk arithmetic).
constexpr uint32_t kMaxSegMs = 2000;
// Frames whose best-fitting note is further than this from the target are rejected by tick().
constexpr float kMaxAcceptCents = 100.0f;

struct Rgb {
    uint8_t r, g, b;
};

// ---- Palettes (full-intensity colours; the cell blends them with black by the display intensity) -------
// The model reports the signed tendency t in [-1, +1] (+ = high / sharp, 0 = centre, see biasT) and a palette
// maps it to a colour. tonemap.live's own colours ({255,170,0}, {34,197,94}, {0,86,214}) are gone.
constexpr int kPaletteCount = 2;
// Palette 0, "Safe" (default): blue - near-white - orange diverging scale for red-green and blue-yellow
// colour vision deficiency. Relative luminance is strictly ordered centre > high > low, so it also reads in
// greyscale / protanopia. centre -> high and centre -> low are linear in RGB.
constexpr Rgb kSafeLow = {0, 100, 255};
constexpr Rgb kSafeCentre = {200, 255, 200};
constexpr Rgb kSafeHigh = {255, 110, 0};
// Palette 1, "Extreme" (device feedback 2026-10-22; ignores colour vision, Safe covers that): the sRGB primaries,
// as far apart as possible, as ONE continuous gradient over the whole range of the cell: lowest end = pure blue,
// exact middle = pure green, highest end = pure red, with luminance-preserving mid-points (green -> cyan -> blue,
// green -> yellow -> red; no brown). It is driven by biasTLinear (a signed LINEAR position, no dead-zone plateau
// and no steep remap), not by biasT. A second rule keeps it vivid:
//  - no dimming: a stained cell shows at least kExtremeMinDisplay of the colour, intensity 0.75 + 0.25 * display
//    (Safe uses the plain display intensity). A never-stained cell stays black.
constexpr Rgb kExtremeCentre = {0, 255, 0};
constexpr Rgb kExtremeHighMid = {255, 255, 0};
constexpr Rgb kExtremeHigh = {255, 0, 0};
constexpr Rgb kExtremeLowMid = {0, 255, 255};
constexpr Rgb kExtremeLow = {0, 0, 255};
constexpr float kExtremeMinDisplay = 0.75f;

// ---- Frequency -> note (pure functions) -------------------------------------------------
// The single place a temperament plugs in: a table of cents offsets from equal temperament per pitch class
// (concert pitch, C = 0), applied on top of the A4 reference. Index 0 is always plain equal temperament.
constexpr int kTemperamentCount = 2;
struct Temperament {
    const char* name;      // shown in the message and the options overlay
    int8_t cents[12];      // offset of each pitch class from equal temperament, C..B
};
extern const Temperament kTemperaments[kTemperamentCount];
// All functions below take the temperament index (default 0 = equal; out-of-range gives equal).
float latticeOffsetCents(int concertMidi, int temperament = 0);
float targetOffsetCents(int concertMidi, float a4, int temperament = 0);  // lattice + 1200*log2(a4/440)
float targetHz(int concertMidi, float a4, int temperament = 0);          // 440*2^((m-69)/12) * 2^(off/1200)

struct NoteHit {
    int concertMidi;  // -1 when hz is not a usable frequency or no candidate lies in 0..127
    float cents;      // 1200*log2(hz/target)
    float errRatio;   // (hz-target)/target
    float targetHz;
};
// +-2 candidates around round(12*log2(hz/a4))+69, minimising |(pos-cand)*100 - lattice(cand)|,
// ties to the lower note. NaN / inf / <= 0 Hz give concertMidi == -1.
NoteHit freqToNote(float hz, float a4, int temperament = 0);

float needlePct(float cents);   // (clamp(c,-50,50)+50)/100; 0 = flat edge of the cell
// Signed tendency t in [-1, +1] (+ = sharp / high) from ToneMap's bias*() formulas: 0 while
// |avgErrRatio * errScale| <= deadzone (and for NaN), else (|e'| - deadzone) / (spanMul * deadzone) clamped to 1.
float biasT(Sensitivity s, float avgErrRatio);
// Colour of palette `palette` (0 Safe, 1 Extreme; anything else is treated as 0) at position t, display
// intensity 1 (for Extreme t is the linear position of biasTLinear: green at 0, yellow / cyan at +-0.5, red / blue
// at +-1). t is clamped to [-1, +1]; NaN gives the centre colour.
Rgb paletteColor(int palette, float t);
// Signed LINEAR position over the whole range for the Extreme palette: clamp(e / (dz + span), -1, +1) with
// e = avgErrRatio * errScale and dz + span = deadzone * (1 + spanMul) (Medium: blue .. green .. red across about
// -15.5 .. 0 .. +15.5 c, Hard +-10.1 c, Relaxed +-18.5 c). No dead zone: exactly 0 (and NaN) is the centre.
float biasTLinear(Sensitivity s, float avgErrRatio);
// LIVE colour of the note sounding now (info strip cents text, LED): the colour of the INSTANTANEOUS deviation
// errRatio (TickResult.errRatio, not the history average and not the Relaxed-smoothed needle) in the active palette:
// Safe -> paletteColor(0, biasT(s, errRatio)) (exact pure centre colour inside the dead zone), Extreme ->
// paletteColor(1, biasTLinear(s, errRatio)). Full intensity: no fade, no 0.75 + 0.25 * display rule, no blending
// with black. NaN gives the centre colour.
Rgb liveColor(int palette, Sensitivity s, float errRatio);
// Colour of a cell with average error avgErrRatio: paletteColor(palette, biasT(...)) for Safe (dead zone) and
// paletteColor(1, biasTLinear(...)) for Extreme.
Rgb biasColor(int palette, Sensitivity s, float avgErrRatio);
// Relative luminance 0..1 of an sRGB colour (Rec. 709 weights on the gamma-encoded channels, i.e. a
// perceptual lightness proxy; linearised luminance would give white text on orange).
float relLuminance(Rgb c);
// Label / marker colour on a cell showing `displayed`: true = black, false = white (luminance > 0.5).
bool useBlackLabel(Rgb displayed);
bool inDeadzone(Sensitivity s, float errRatio);   // |errRatio| <= deadzone (raw, unscaled)
float baseIntensity(uint32_t totalMs);            // min(1, total/250)

struct TickResult {
    bool hasNote;       // a note is active after this tick
    bool newNote;       // it differs from the previous frame's note (or there was none)
    int concertMidi;    // -1 when !hasNote
    int writtenMidi;    // concertMidi - transposition (may fall outside 0..127)
    float hz;           // input frequency (0 when !hasNote)
    float cents;        // raw cents of this frame
    float displayCents; // Medium / Hard: raw; Relaxed: EMA with tau 350 ms
    float errRatio;     // raw, relative to the current target
    bool inDeadzone;    // from the raw errRatio
};

// ---- Per-note state (docs/TONEMAP_DESIGN.md section 5.2) --------------------------------
struct Chunk {
    uint16_t durMs;
    int16_t errQ16;  // average stored error * 65536
};

struct NoteState {
    int64_t sumAll;        // sum(dur_ms * errQ16) over all voiced time (Unlimited history)
    uint32_t totalMs;      // voiced ms since reset (saturating)
    uint32_t lastStainMs;
    float displayI;        // display intensity as of the last stain / recompute (Relaxed uses it)
    float fadeAnchor;      // reserved: the lazy fade starts from displayI, so this stays 0
    int32_t accFineSum;    // open fine chunk: sum(dur * errQ16)
    int32_t accCoarseSum;  // open coarse chunk
    uint16_t accFineMs;
    uint16_t accCoarseMs;
    uint8_t fineHead, fineCnt, coarseHead, coarseCnt;  // head = next write index
    uint8_t flags;         // kVisited | kAnchorValid (reserved) | kEverStained
    uint8_t pad;
    Chunk fine[kFineCap];
    Chunk coarse[kCoarseCap];
};
static_assert(sizeof(Chunk) == 4, "Chunk must be 4 bytes");
static_assert(sizeof(NoteState) == 296, "NoteState must be 296 bytes (docs/TONEMAP_DESIGN.md 5.2)");

constexpr uint8_t kFlagVisited = 1, kFlagAnchorValid = 2, kFlagEverStained = 4;

class Model {
public:
    Model() {}
    // Clears every note and the active note; keeps sensitivity / history / hold / palette / A4 / transposition.
    void reset(uint32_t nowMs);

    // The five setters below apply ToneMap's recompute side effects to every stained note:
    // the display intensity is materialised first (Relaxed keeps its value, Medium / Hard jump to
    // the base intensity), then lastStain = now (the hold timer restarts).
    void setSensitivity(Sensitivity s, uint32_t nowMs);
    void setHistory(int idx, uint32_t nowMs);
    void setRefA(float a4, uint32_t nowMs);      // 400..480 expected; non-finite / <= 0 ignored
    void setTransposition(int semis, uint32_t nowMs);  // also ends the active note
    // Color hold change: no timer restart. nowMs is needed to materialise a Relaxed fade in
    // progress (deviation from the design signature setHold(int)).
    void setHold(int idx, uint32_t nowMs);
    // Palette (0 Safe, 1 Extreme; clamped). Only changes how colours are computed: no timers, no data.
    void setPalette(int idx);
    // Temperament (index into kTemperaments; clamped). The stored errors are relative to the old targets, so a real
    // change clears the whole map (and ends the active note). Same index: nothing happens.
    void setTemperament(int idx, uint32_t nowMs);

    // One analysis frame. `valid` = the caller's gates passed (clarity, level, 14 < hz < 8200).
    // An invalid frame, or a non-finite / <= 0 hz, ends the active note. dtMs = time since the
    // previous frame; a frame produces a history segment of dtMs only when the note is the same
    // as in the previous valid frame (the first frame of a note produces none) and dtMs > 0.
    TickResult tick(bool valid, float hz, uint32_t dtMs, uint32_t nowMs);
    void endNote();  // pause / leave: next valid frame starts a new note

    // Colour of the cell for a WRITTEN midi at time nowMs (lazy fade evaluated here), in the current palette.
    // Black when the display intensity is <= 0.001 or the note is outside 0..127.
    Rgb cellColor(int writtenMidi, uint32_t nowMs, bool* visited) const;
    // True while any note is inside (or just after, +kFadeSettleMs) its 10 s fade ramp.
    bool anyFading(uint32_t nowMs) const;

    // Test hooks / accessors.
    float avgErrRatio(int concertMidi) const;  // history-window average, relative to the current A4
    uint32_t totalMs(int concertMidi) const;
    float displayIntensity(int concertMidi, uint32_t nowMs) const;
    const NoteState* note(int concertMidi) const;
    Sensitivity sensitivity() const { return sens_; }
    int historyIdx() const { return histIdx_; }
    int holdIdx() const { return holdIdx_; }
    int palette() const { return palette_; }
    int temperament() const { return temper_; }
    float refA() const { return a4_; }
    int transposition() const { return transp_; }
    int activeConcertMidi() const { return activeMidi_; }

private:
    float evalDisplay(const NoteState& s, uint32_t nowMs) const;
    void materialiseAll(uint32_t nowMs);   // before a setting changes
    void restartTimers(uint32_t nowMs);    // after it changed
    void addSegment(NoteState& s, uint32_t dtMs, float errRatio, uint32_t nowMs);
    float windowAvgStored(const NoteState& s) const;

    NoteState notes_[kNoteCount];
    Sensitivity sens_ = Sensitivity::Medium;
    uint8_t histIdx_ = 1;
    uint8_t holdIdx_ = 2;
    uint8_t palette_ = 0;
    uint8_t temper_ = 0;
    int8_t transp_ = 0;
    float a4_ = 440.0f;
    int activeMidi_ = -1;       // concert midi of the note of the previous valid frame
    float needleCents_ = 0.0f;  // Relaxed EMA state
    bool needleValid_ = false;
};

constexpr uint32_t kFadeSettleMs = 600;

}  // namespace tonemap
