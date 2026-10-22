// ToneMap heat-map model. See tonemap_model.h.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
#include "tonemap_model.h"

#include <math.h>
#include <string.h>

namespace tonemap {

// spanMul is 2 / 2 / 4 (tonemap.live: 4 / 4 / 8): deliberate deviation, narrower ramp = bolder colours.
const SensConst kSens[3] = {
    {0.0025f, 0.7f, 2.0f, 350.0f},    // Relaxed
    {0.0021f, 0.7f, 2.0f, 80.0f},     // Medium
    {0.00105f, 0.9f, 4.0f, 30.0f},    // Hard
};

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool finitef(float v) { return v == v && fabsf(v) <= 3.4e38f; }

inline int16_t clampQ16(float stored) {
    const float q = roundf(stored * 65536.0f);
    if (!(q > -32768.0f)) return -32768;  // also catches NaN
    if (q > 32767.0f) return 32767;
    return (int16_t)q;
}

// Rounded integer division that rounds half away from zero; den > 0.
inline int32_t divRound(int32_t num, int32_t den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

struct RgbF {
    float r, g, b;
};

inline RgbF toF(const Rgb& c) { return {(float)c.r, (float)c.g, (float)c.b}; }

// from + (to - from) * u, per channel
inline RgbF lerpRgb(const Rgb& from, const Rgb& to, float u) {
    RgbF o = toF(from);
    o.r += ((float)to.r - o.r) * u;
    o.g += ((float)to.g - o.g) * u;
    o.b += ((float)to.b - o.b) * u;
    return o;
}

// Palette colour at tendency t (clamped to [-1, +1]), full intensity.
RgbF paletteF(int palette, float t) {
    if (!(t == t)) t = 0.0f;
    t = clampf(t, -1.0f, 1.0f);
    if (palette == 1) {  // Classic: green -> yellow -> red / green -> cyan -> blue
        const float a = t < 0.0f ? -t : t;  // distance from the centre, 0..1
        const bool high = t >= 0.0f;
        const Rgb& mid = high ? kClassicHighMid : kClassicLowMid;
        const Rgb& end = high ? kClassicHigh : kClassicLow;
        return a <= 0.5f ? lerpRgb(kClassicCentre, mid, a * 2.0f) : lerpRgb(mid, end, (a - 0.5f) * 2.0f);
    }
    // Safe (default): centre -> high / centre -> low, linear in RGB.
    return t >= 0.0f ? lerpRgb(kSafeCentre, kSafeHigh, t) : lerpRgb(kSafeCentre, kSafeLow, -t);
}

inline uint8_t toByte(float v) {
    const float r = roundf(v);
    return (uint8_t)(r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r));
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Pure functions
// ---------------------------------------------------------------------------------------
float latticeOffsetCents(int /*concertMidi*/) { return 0.0f; }

float targetOffsetCents(int concertMidi, float a4) {
    // getTargetOffsetCents = getLatticeOffsetCents + getA4OffsetCents
    return latticeOffsetCents(concertMidi) + 1200.0f * log2f(a4 / 440.0f);
}

float targetHz(int concertMidi, float a4) {
    const float equalTemp = 440.0f * exp2f((float)(concertMidi - 69) / 12.0f);
    return equalTemp * exp2f(targetOffsetCents(concertMidi, a4) / 1200.0f);
}

NoteHit freqToNote(float hz, float a4) {
    NoteHit h = {-1, 0.0f, 0.0f, 0.0f};
    if (!finitef(hz) || !(hz > 0.0f) || !finitef(a4) || !(a4 > 0.0f)) return h;

    const float semis = 12.0f * log2f(hz / a4);
    if (!finitef(semis) || fabsf(semis) > 400.0f) return h;
    const float latticePos = semis + 69.0f;
    const int mid = (int)floorf(semis + 0.5f) + 69;  // Math.round
    float bestDev = 3.4e38f;
    int best = -1;
    for (int cand = mid - 2; cand <= mid + 2; ++cand) {
        if (cand < 0 || cand > 127) continue;
        const float dev = fabsf((latticePos - (float)cand) * 100.0f - latticeOffsetCents(cand));
        if (dev < bestDev) {  // strict: ties stay with the lower note
            bestDev = dev;
            best = cand;
        }
    }
    if (best < 0) return h;

    const float t = targetHz(best, a4);
    h.concertMidi = best;
    h.targetHz = t;
    h.cents = 1200.0f * log2f(hz / t);
    h.errRatio = (hz - t) / t;
    return h;
}

float needlePct(float cents) {
    if (!(cents == cents)) cents = 0.0f;
    return (clampf(cents, -50.0f, 50.0f) + 50.0f) / 100.0f;
}

// ToneMap bias*(): signed ramp position. 0 inside the deadzone (and for NaN), else a linear ramp up to
// +-1 over spanMul * deadzone. + = sharp (high), - = flat (low).
float biasT(Sensitivity s, float avgErrRatio) {
    const SensConst& c = kSens[(int)s];
    const float scaled = avgErrRatio * c.errScale;
    const float a = fabsf(scaled);
    if (!(a > c.deadzone)) return 0.0f;  // NaN -> centre
    const float span = c.spanMul * c.deadzone;
    const float t = clampf((a - c.deadzone) / span, 0.0f, 1.0f);
    return scaled > 0.0f ? t : -t;
}

Rgb paletteColor(int palette, float t) {
    const RgbF f = paletteF(palette, t);
    Rgb o = {toByte(f.r), toByte(f.g), toByte(f.b)};
    return o;
}

Rgb biasColor(int palette, Sensitivity s, float avgErrRatio) { return paletteColor(palette, biasT(s, avgErrRatio)); }

float relLuminance(Rgb c) { return (0.2126f * (float)c.r + 0.7152f * (float)c.g + 0.0722f * (float)c.b) / 255.0f; }

bool useBlackLabel(Rgb displayed) { return relLuminance(displayed) > 0.5f; }

bool inDeadzone(Sensitivity s, float errRatio) { return fabsf(errRatio) <= kSens[(int)s].deadzone; }

float baseIntensity(uint32_t totalMs) {
    const float v = (float)totalMs / kFullConfMs;
    return v > 1.0f ? 1.0f : v;
}

// ---------------------------------------------------------------------------------------
// Model: settings
// ---------------------------------------------------------------------------------------
void Model::reset(uint32_t /*nowMs*/) {
    memset(notes_, 0, sizeof(notes_));
    activeMidi_ = -1;
    needleValid_ = false;
    needleCents_ = 0.0f;
}

// Lazy display intensity of a note (ToneMap updateCellDisplayColor, evaluated on demand).
// Medium / Hard: display = base. Relaxed: the stored value (EMA towards base only on stain frames).
// After `hold` since the last stain, a linear 10 s ramp from the stored value to 0.45*base;
// it stays there. No fade (hold < 0): no ramp. Never stained or base == 0: 0.
float Model::evalDisplay(const NoteState& s, uint32_t nowMs) const {
    if (!(s.flags & kFlagEverStained)) return 0.0f;
    const float base = baseIntensity(s.totalMs);
    if (base <= 0.0f) return 0.0f;
    float d = (sens_ == Sensitivity::Relaxed) ? s.displayI : base;
    d = clampf(d, 0.0f, 1.0f);
    const int32_t hold = kHoldMs[holdIdx_];
    if (hold < 0) return d;
    const uint32_t el = nowMs >= s.lastStainMs ? nowMs - s.lastStainMs : 0;
    if (el <= (uint32_t)hold) return d;
    const float target = base * kStainMinFraction;
    const uint32_t into = el - (uint32_t)hold;
    if (into >= kFadeMs) return d < target ? d : target;
    const float p = (float)into / (float)kFadeMs;
    return clampf(d + (target - d) * p, 0.0f, 1.0f);
}

// Before a setting that changes how the display is evaluated: store what is shown now.
void Model::materialiseAll(uint32_t nowMs) {
    for (int i = 0; i < kNoteCount; ++i) {
        NoteState& s = notes_[i];
        if (!(s.flags & kFlagEverStained)) continue;
        s.displayI = evalDisplay(s, nowMs);
    }
}

// ToneMap recomputeAllCellColorsAndRender: lastStain = now, anchor cleared (the lazy fade has
// no separate anchor). Medium / Hard evaluate to base from now on; Relaxed keeps displayI.
void Model::restartTimers(uint32_t nowMs) {
    for (int i = 0; i < kNoteCount; ++i) {
        NoteState& s = notes_[i];
        if (!(s.flags & kFlagEverStained)) continue;
        s.lastStainMs = nowMs;
        s.flags &= (uint8_t)~kFlagAnchorValid;
        s.fadeAnchor = 0.0f;
    }
}

void Model::setSensitivity(Sensitivity s, uint32_t nowMs) {
    materialiseAll(nowMs);
    sens_ = s;
    needleValid_ = false;
    restartTimers(nowMs);
}

void Model::setHistory(int idx, uint32_t nowMs) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    materialiseAll(nowMs);
    histIdx_ = (uint8_t)idx;
    restartTimers(nowMs);
}

void Model::setRefA(float a4, uint32_t nowMs) {
    if (!finitef(a4) || !(a4 > 0.0f)) return;
    materialiseAll(nowMs);
    a4_ = a4;
    restartTimers(nowMs);
}

void Model::setTransposition(int semis, uint32_t nowMs) {
    if (semis < -127) semis = -127;
    if (semis > 127) semis = 127;
    endNote();
    materialiseAll(nowMs);
    transp_ = (int8_t)semis;
    restartTimers(nowMs);
}

void Model::setHold(int idx, uint32_t nowMs) {
    if (idx < 0) idx = 0;
    if (idx > 5) idx = 5;
    materialiseAll(nowMs);  // no restartTimers: the hold clock keeps running
    holdIdx_ = (uint8_t)idx;
}

void Model::setPalette(int idx) {
    if (idx < 0) idx = 0;
    if (idx > kPaletteCount - 1) idx = kPaletteCount - 1;
    palette_ = (uint8_t)idx;  // colours are computed on demand: nothing to materialise or restart
}

void Model::endNote() {
    activeMidi_ = -1;
    needleValid_ = false;
    needleCents_ = 0.0f;
}

// ---------------------------------------------------------------------------------------
// Model: history chunks
// ---------------------------------------------------------------------------------------
namespace {

void closeCoarse(NoteState& s) {
    if (s.accCoarseMs == 0) return;
    Chunk c;
    c.durMs = s.accCoarseMs;
    c.errQ16 = (int16_t)divRound(s.accCoarseSum, (int32_t)s.accCoarseMs);
    s.coarse[s.coarseHead] = c;
    s.coarseHead = (uint8_t)((s.coarseHead + 1) % kCoarseCap);
    if (s.coarseCnt < kCoarseCap) ++s.coarseCnt;
    s.accCoarseSum = 0;
    s.accCoarseMs = 0;
}

// The oldest fine chunk leaves the fine ring and joins the open coarse chunk.
void spillToCoarse(NoteState& s, const Chunk& c) {
    s.accCoarseSum += (int32_t)c.durMs * (int32_t)c.errQ16;
    s.accCoarseMs = (uint16_t)(s.accCoarseMs + c.durMs);
    if (s.accCoarseMs >= kCoarseChunkMs) closeCoarse(s);
}

void closeFine(NoteState& s) {
    if (s.accFineMs == 0) return;
    Chunk c;
    c.durMs = s.accFineMs;
    c.errQ16 = (int16_t)divRound(s.accFineSum, (int32_t)s.accFineMs);
    s.accFineSum = 0;
    s.accFineMs = 0;
    if (s.fineCnt == kFineCap) {
        const int oldest = s.fineHead;  // head == oldest entry when the ring is full
        spillToCoarse(s, s.fine[oldest]);
    }
    s.fine[s.fineHead] = c;
    s.fineHead = (uint8_t)((s.fineHead + 1) % kFineCap);
    if (s.fineCnt < kFineCap) ++s.fineCnt;
}

}  // namespace

// One segment of dtMs (>0) with the frame's errRatio, relative to the current A4 target.
void Model::addSegment(NoteState& s, uint32_t dtMs, float errRatio, uint32_t nowMs) {
    if (dtMs > kMaxSegMs) dtMs = kMaxSegMs;

    // Value shown before this stain (Relaxed EMA starts from it, fade included).
    const float prevDisplay = evalDisplay(s, nowMs);

    // Stored error is relative to A=440: stored = (1+err)*(a4/440) - 1 (docs section 5.4).
    const float stored = (1.0f + errRatio) * (a4_ / 440.0f) - 1.0f;
    const int32_t q = clampQ16(stored);
    const int32_t dt = (int32_t)dtMs;

    s.accFineSum += dt * q;
    s.accFineMs = (uint16_t)(s.accFineMs + dt);
    uint32_t room = 0xFFFFFFFFu - s.totalMs;
    const uint32_t dAll = dtMs < room ? dtMs : room;  // saturating total; Unlimited stays consistent
    s.totalMs += dAll;
    s.sumAll += (int64_t)dAll * (int64_t)q;
    if (s.accFineMs >= kFineChunkMs) closeFine(s);

    const float base = baseIntensity(s.totalMs);
    float d;
    if (sens_ == Sensitivity::Relaxed) {
        // Deviation from ToneMap (design 0): alpha uses this tick's dt, not the render interval.
        const float alpha = 1.0f - expf(-(float)dtMs / kFadeInMs);
        d = prevDisplay + (base - prevDisplay) * alpha;
    } else {
        d = base;
    }
    s.displayI = clampf(d, 0.0f, 1.0f);
    s.lastStainMs = nowMs;
    s.flags |= (uint8_t)(kFlagEverStained | kFlagVisited);
    s.flags &= (uint8_t)~kFlagAnchorValid;
}

// Average stored error (ratio, relative to A=440) over the History window of this note.
float Model::windowAvgStored(const NoteState& s) const {
    if (s.totalMs == 0) return 0.0f;
    const uint32_t hMs = kHistoryMs[histIdx_];
    if (hMs == 0) return (float)s.sumAll / (float)s.totalMs / 65536.0f;  // Unlimited, exact

    // Newest to oldest: open fine, fine ring, open coarse, coarse ring. The edge chunk enters
    // fractionally (Math.min(seg.durMs, remaining) in getRecentWeightedErr).
    int64_t wholeSum = 0;  // exact part: sum(dur * errQ16) of whole elements
    float partial = 0.0f;  // the fractional edge element
    uint32_t rem = hMs, total = 0;

    if (s.accFineMs > 0 && rem > 0) {
        if (s.accFineMs <= rem) {
            wholeSum += s.accFineSum;
            total += s.accFineMs;
            rem -= s.accFineMs;
        } else {
            partial += (float)s.accFineSum * (float)rem / (float)s.accFineMs;
            total += rem;
            rem = 0;
        }
    }
    for (int i = 0; i < s.fineCnt && rem > 0; ++i) {
        const Chunk& c = s.fine[(s.fineHead + kFineCap - 1 - i) % kFineCap];
        const uint32_t take = c.durMs < rem ? c.durMs : rem;
        if (take == c.durMs) wholeSum += (int64_t)c.durMs * c.errQ16;
        else partial += (float)take * (float)c.errQ16;
        total += take;
        rem -= take;
    }
    if (s.accCoarseMs > 0 && rem > 0) {
        if (s.accCoarseMs <= rem) {
            wholeSum += s.accCoarseSum;
            total += s.accCoarseMs;
            rem -= s.accCoarseMs;
        } else {
            partial += (float)s.accCoarseSum * (float)rem / (float)s.accCoarseMs;
            total += rem;
            rem = 0;
        }
    }
    for (int i = 0; i < s.coarseCnt && rem > 0; ++i) {
        const Chunk& c = s.coarse[(s.coarseHead + kCoarseCap - 1 - i) % kCoarseCap];
        const uint32_t take = c.durMs < rem ? c.durMs : rem;
        if (take == c.durMs) wholeSum += (int64_t)c.durMs * c.errQ16;
        else partial += (float)take * (float)c.errQ16;
        total += take;
        rem -= take;
    }
    if (total == 0) return 0.0f;
    return ((float)wholeSum + partial) / (float)total / 65536.0f;
}

float Model::avgErrRatio(int concertMidi) const {
    if (concertMidi < 0 || concertMidi >= kNoteCount) return 0.0f;
    const float stored = windowAvgStored(notes_[concertMidi]);
    // Re-base from the A=440 reference to the current A4 (affine, exact for the average).
    return (1.0f + stored) * (440.0f / a4_) - 1.0f;
}

uint32_t Model::totalMs(int concertMidi) const {
    if (concertMidi < 0 || concertMidi >= kNoteCount) return 0;
    return notes_[concertMidi].totalMs;
}

float Model::displayIntensity(int concertMidi, uint32_t nowMs) const {
    if (concertMidi < 0 || concertMidi >= kNoteCount) return 0.0f;
    return evalDisplay(notes_[concertMidi], nowMs);
}

const NoteState* Model::note(int concertMidi) const {
    if (concertMidi < 0 || concertMidi >= kNoteCount) return nullptr;
    return &notes_[concertMidi];
}

// ---------------------------------------------------------------------------------------
// Model: tick, colours
// ---------------------------------------------------------------------------------------
TickResult Model::tick(bool valid, float hz, uint32_t dtMs, uint32_t nowMs) {
    TickResult r = {false, false, -1, 0, 0.0f, 0.0f, 0.0f, 0.0f, false};

    NoteHit hit = {-1, 0.0f, 0.0f, 0.0f};
    if (valid) {
        hit = freqToNote(hz, a4_);
        if (hit.concertMidi >= 0 && !(fabsf(hit.cents) <= kMaxAcceptCents)) hit.concertMidi = -1;
    }
    if (hit.concertMidi < 0) {
        endNote();  // one invalid frame ends the note: no histeresis (brief 3.6)
        return r;
    }

    const bool sameNote = (hit.concertMidi == activeMidi_);
    NoteState& s = notes_[hit.concertMidi];
    s.flags |= kFlagVisited;  // the active cell counts as visited (highlightGridCell)

    // Needle: Relaxed smooths with an EMA (tau 350 ms), Medium / Hard show the raw cents.
    // The EMA restarts on a note change or after an invalid frame.
    float displayCents = hit.cents;
    if (sens_ == Sensitivity::Relaxed) {
        if (!sameNote || !needleValid_) {
            needleCents_ = hit.cents;
        } else {
            const float alpha = 1.0f - expf(-(float)dtMs / kSens[(int)sens_].needleTauMs);
            needleCents_ += (hit.cents - needleCents_) * alpha;
        }
        needleValid_ = true;
        displayCents = needleCents_;
    } else {
        needleValid_ = false;
    }

    // History: the first valid frame of a note produces no segment; later frames of the same
    // note produce one of dtMs (none when dtMs == 0).
    if (sameNote && dtMs > 0) addSegment(s, dtMs, hit.errRatio, nowMs);

    activeMidi_ = hit.concertMidi;

    r.hasNote = true;
    r.newNote = !sameNote;
    r.concertMidi = hit.concertMidi;
    r.writtenMidi = hit.concertMidi - (int)transp_;
    r.hz = hz;
    r.cents = hit.cents;
    r.displayCents = displayCents;
    r.errRatio = hit.errRatio;
    r.inDeadzone = inDeadzone(sens_, hit.errRatio);
    return r;
}

Rgb Model::cellColor(int writtenMidi, uint32_t nowMs, bool* visited) const {
    const Rgb black = {0, 0, 0};
    if (visited) *visited = false;
    const int concert = writtenMidi + (int)transp_;
    if (concert < 0 || concert >= kNoteCount) return black;
    const NoteState& s = notes_[concert];
    if (visited) *visited = (s.flags & kFlagVisited) != 0;
    const float display = evalDisplay(s, nowMs);
    if (display <= 0.001f) return black;
    const RgbF bias = paletteF(palette_, biasT(sens_, avgErrRatio(concert)));
    // lerp(background = black, bias, display), rounded per channel
    Rgb o = {toByte(bias.r * display), toByte(bias.g * display), toByte(bias.b * display)};
    return o;
}

bool Model::anyFading(uint32_t nowMs) const {
    const int32_t hold = kHoldMs[holdIdx_];
    if (hold < 0) return false;
    for (int i = 0; i < kNoteCount; ++i) {
        const NoteState& s = notes_[i];
        if (!(s.flags & kFlagEverStained)) continue;
        if (baseIntensity(s.totalMs) <= 0.0f) continue;
        const uint32_t el = nowMs >= s.lastStainMs ? nowMs - s.lastStainMs : 0;
        if (el > (uint32_t)hold && el - (uint32_t)hold < kFadeMs + kFadeSettleMs) return true;
    }
    return false;
}

}  // namespace tonemap
