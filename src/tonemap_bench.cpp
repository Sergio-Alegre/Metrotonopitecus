// ToneMap bench firmware (env cardputer-adv-tonemap-bench, -DTONEMAP_BENCH=1).
// Replaces the normal app: it measures the detector and the capture path and prints
// everything over serial, in English, one parseable line per fact.
//
//   [bench]  header: build, CPU, heap, buffer sizes
//   [synth]  detector accuracy on generated signals, 16 and 32 kHz, then a SUMMARY per rate
//   [time]   microseconds per analysis (avg / max of 50 runs)
//   [test]   ToneMap model tests (parity vectors, colour, history, fade, re-basing, transposition,
//            chunk equivalence, first-frame rule, robustness, budget): PASS / FAIL per series
//   [live]   microphone mode, ~10 lines/s
//   [clock]  measured I2S chunk rate every 10 s (real sample rate vs nominal)
//   [load]   draw-load toggle messages
//
// Serial keys (send one character):  d  toggle simulated draw load (240x125 sprite every 50 ms)
//                                    r  re-print header + test + SUMMARY + timing lines
//                                    a  re-print every synthetic case line
// The header, SUMMARY and timing lines are also re-printed every 10 s so that a monitor
// attached late still sees them.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
#ifdef TONEMAP_BENCH

#include <M5Cardputer.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "audio.h"
#include "pitch_mpm.h"
#include "tonemap_audio.h"
#include "tonemap_model.h"
#include "tonemap_vectors.h"  // generated into test/ by tools/gen_tonemap_vectors.py

namespace {

constexpr size_t kN = tonemap_audio::kWindow;  // 2048
constexpr float kGateRms = 120.0f;             // "Low" gate of the design (int16 units)
constexpr float kMinClarity = 0.8f;
constexpr int kLiveMaxLag = 720;               // 44.4 Hz at 32 kHz

int16_t sig[kN];  // synthetic signal / live window
float work[kN];
float nsdf[mpm::kMaxLag + 2];

// ---------------------------------------------------------------------------------------
// Report lines (header, SUMMARY, timing): stored so they can be re-printed.
// ---------------------------------------------------------------------------------------
constexpr int kMaxReport = 64;
constexpr int kLineLen = 220;
char report[kMaxReport][kLineLen];
int reportCount = 0;

void rep(const char* fmt, ...) {
    char tmp[kLineLen];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (reportCount < kMaxReport) strlcpy(report[reportCount++], tmp, kLineLen);
    Serial.println(tmp);
}

void reprintReport() {
    for (int i = 0; i < reportCount; ++i) Serial.println(report[i]);
}

// ---------------------------------------------------------------------------------------
// Note helpers
// ---------------------------------------------------------------------------------------
const char* kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void noteName(int midi, char* out, size_t cap) {
    if (midi < 0 || midi > 127) {
        strlcpy(out, "--", cap);
        return;
    }
    snprintf(out, cap, "%s%d", kSharpNames[midi % 12], midi / 12 - 1);
}

// ---------------------------------------------------------------------------------------
// Signal generation (double precision: the bench is a test tool, correctness first)
// ---------------------------------------------------------------------------------------
// Sine, or band-limited sawtooth sum_{k=1..H} sin(k w n + k phase) / k with H = min(10, 0.45 fs / f).
// Each harmonic is a rotating phasor (recurrence in double, drift ~1e-13 over 2048 steps).
void genSignal(int16_t* out, size_t n, double fs, double f, bool saw, double phase0) {
    int H = 1;
    if (saw) {
        H = (int)(0.45 * fs / f);
        if (H < 1) H = 1;
        if (H > 10) H = 10;
    }
    double s[10], c[10], rs[10], rc[10], amp[10];
    const double w = 2.0 * M_PI * f / fs;
    const double scale = saw ? 3500.0 : 8000.0;
    for (int k = 0; k < H; ++k) {
        const double a0 = (k + 1) * phase0;
        s[k] = sin(a0);
        c[k] = cos(a0);
        rs[k] = sin((k + 1) * w);
        rc[k] = cos((k + 1) * w);
        amp[k] = scale / (k + 1);
    }
    for (size_t i = 0; i < n; ++i) {
        double v = 0.0;
        for (int k = 0; k < H; ++k) {
            v += amp[k] * s[k];
            const double ns = s[k] * rc[k] + c[k] * rs[k];
            const double nc = c[k] * rc[k] - s[k] * rs[k];
            s[k] = ns;
            c[k] = nc;
        }
        out[i] = (int16_t)lround(v);
    }
}

uint32_t rngState = 0x12345678u;
uint32_t rngNext() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}
int rngSigned(int amp) { return (int)(rngNext() % (uint32_t)(2 * amp + 1)) - amp; }

// ---------------------------------------------------------------------------------------
// Synthetic accuracy
// ---------------------------------------------------------------------------------------
struct RateCfg {
    uint32_t fs;
    int maxLag;
};
const RateCfg kRates[2] = {{32000, 720}, {16000, 360}};

struct NoteCfg {
    const char* name;
    int midi;
};
const NoteCfg kNotes[10] = {{"C2", 36}, {"A2", 45}, {"C3", 48}, {"C4", 60}, {"A4", 69},
                            {"C5", 72}, {"C6", 84}, {"C7", 96}, {"A7", 105}, {"B7", 107}};
const int kCents[3] = {-20, 0, 20};
const char* kWaveName[2] = {"sine", "saw"};

struct CaseResult {
    uint8_t rate, wave, note, centsIdx;
    bool found, octErr, pass;
    float fexp, fdet, errCents, clarity;
};
constexpr int kCaseCount = 2 * 2 * 10 * 3;
CaseResult cases[kCaseCount];

void printCase(const CaseResult& cr) {
    const RateCfg& rc = kRates[cr.rate];
    Serial.printf(
        "[synth] fs=%u N=%u maxLag=%d wave=%s note=%s off=%+dc exp=%.2f det=%.2f err=%+.2fc clar=%.3f oct=%s %s\n",
        (unsigned)rc.fs, (unsigned)kN, rc.maxLag, kWaveName[cr.wave], kNotes[cr.note].name, kCents[cr.centsIdx], cr.fexp,
        cr.found ? cr.fdet : 0.0f, cr.found ? cr.errCents : 0.0f, cr.found ? cr.clarity : 0.0f,
        !cr.found ? "none" : (cr.octErr ? "ERR" : "ok"), cr.pass ? "PASS" : "FAIL");
}

void reprintCases() {
    for (int i = 0; i < kCaseCount; ++i) printCase(cases[i]);
}

void runSynthetic() {
    int idx = 0;
    for (int ri = 0; ri < 2; ++ri) {
        const RateCfg& rc = kRates[ri];
        int nCases = 0, octErrors = 0, fails = 0;
        float maxLow = 0, maxOct7 = 0, maxOct7ByWave[2] = {0, 0};
        for (int wave = 0; wave < 2; ++wave) {
            for (int ni = 0; ni < 10; ++ni) {
                for (int ci = 0; ci < 3; ++ci) {
                    const double fexp = 440.0 * pow(2.0, (kNotes[ni].midi - 69) / 12.0 + kCents[ci] / 1200.0);
                    genSignal(sig, kN, (double)rc.fs, fexp, wave == 1, 0.37 + 0.71 * ni);
                    const mpm::Result r = mpm::detect(sig, kN, (float)rc.fs, rc.maxLag, work, nsdf);

                    CaseResult& cr = cases[idx++];
                    cr.rate = (uint8_t)ri;
                    cr.wave = (uint8_t)wave;
                    cr.note = (uint8_t)ni;
                    cr.centsIdx = (uint8_t)ci;
                    cr.found = r.found;
                    cr.fexp = (float)fexp;
                    cr.fdet = r.freq;
                    cr.clarity = r.clarity;
                    cr.errCents = 0;
                    cr.octErr = false;
                    cr.pass = false;
                    if (r.found) {
                        const double err = 1200.0 * log2((double)r.freq / fexp);
                        cr.errCents = (float)err;
                        const double k = round(err / 1200.0);
                        cr.octErr = (k != 0.0) && fabs(err - 1200.0 * k) < 100.0;
                        const bool oct7 = kNotes[ni].midi >= 96;
                        const double lim = oct7 ? 3.0 : 1.0;
                        cr.pass = !cr.octErr && fabs(err) <= lim && r.clarity > kMinClarity;
                        if (!cr.octErr) {
                            const float a = (float)fabs(err);
                            if (oct7) {
                                if (a > maxOct7) maxOct7 = a;
                                if (a > maxOct7ByWave[wave]) maxOct7ByWave[wave] = a;
                            } else if (a > maxLow) {
                                maxLow = a;
                            }
                        }
                    }
                    ++nCases;
                    if (cr.octErr) ++octErrors;
                    if (!cr.pass) ++fails;
                    printCase(cr);
                    delay(1);  // let the USB serial drain
                }
            }
        }
        const char* verdict = "PASS";
        if (octErrors > 0) verdict = "FAIL(octave)";
        else if (maxLow > 1.0f) verdict = "FAIL(<=C6)";
        else if (maxOct7 > 3.0f) verdict = "BESTEFFORT(oct7)";
        rep("[synth] SUMMARY fs=%u maxLag=%d: cases=%d octave_errors=%d max_err_le_C6=%.2fc max_err_oct7=%.2fc "
            "(sine %.2fc, saw %.2fc) case_fails=%d %s",
            (unsigned)rc.fs, rc.maxLag, nCases, octErrors, maxLow, maxOct7, maxOct7ByWave[0], maxOct7ByWave[1], fails,
            verdict);
    }
}

// ---------------------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------------------
// One analysis = prepare + (gate) + findPitch, as the view will run it.
void timeCase(const char* label, const RateCfg& rc) {
    constexpr int kRuns = 50;
    // One untimed run first: the code is fetched from flash through the cache.
    {
        const mpm::Levels lv = mpm::prepare(sig, kN, work);
        if (lv.rms >= kGateRms) mpm::findPitch(work, kN, (float)rc.fs, rc.maxLag, nsdf);
    }
    uint32_t sum = 0, mx = 0, prepSum = 0;
    bool gated = false;
    float rms = 0;
    for (int i = 0; i < kRuns; ++i) {
        const uint32_t t0 = micros();
        const mpm::Levels lv = mpm::prepare(sig, kN, work);
        const uint32_t t1 = micros();
        gated = lv.rms < kGateRms;
        rms = lv.rms;
        if (!gated) mpm::findPitch(work, kN, (float)rc.fs, rc.maxLag, nsdf);
        const uint32_t t2 = micros();
        sum += t2 - t0;
        prepSum += t1 - t0;
        if (t2 - t0 > mx) mx = t2 - t0;
    }
    const uint32_t avg = sum / kRuns;
    rep("[time] fs=%u maxLag=%d case=%s runs=%d avg_us=%u max_us=%u prep_avg_us=%u gated=%d rms=%.0f %s",
        (unsigned)rc.fs, rc.maxLag, label, kRuns, (unsigned)avg, (unsigned)mx, (unsigned)(prepSum / kRuns), (int)gated, rms,
        mx < 20000 ? "OK(<20ms)" : "SLOW(>=20ms)");
}

void runTiming() {
    for (int ri = 0; ri < 2; ++ri) {
        const RateCfg& rc = kRates[ri];
        // 1. sawtooth at C3 (typical voiced window)
        genSignal(sig, kN, (double)rc.fs, 440.0 * pow(2.0, (48 - 69) / 12.0), true, 0.37);
        timeCase("saw_C3", rc);
        // 2. worst case: noise plus a 50 Hz tone (lag 640 at 32 kHz): many key maxima, long lags
        genSignal(sig, kN, (double)rc.fs, 50.0, false, 0.5);
        for (size_t i = 0; i < kN; ++i) {
            int v = sig[i] / 2 + rngSigned(5000);
            sig[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        timeCase("noise_plus_50Hz", rc);
        // 3. silence below the gate: DC offset plus a little noise; findPitch is skipped
        for (size_t i = 0; i < kN; ++i) sig[i] = (int16_t)(300 + rngSigned(20));
        timeCase("silence_gated", rc);
        delay(1);
    }
}

// ---------------------------------------------------------------------------------------
// ToneMap model tests (docs/TONEMAP_DESIGN.md section 9)
// ---------------------------------------------------------------------------------------
// Every expected value below is derived from ToneMap's formulas evaluated in double precision
// (and cross-checked with an independent Python re-implementation), never copied from the
// model's own output:
//
//   ratio(c) = 2^(c/1200) - 1                        cents -> errRatio
//   bias     e' = e*scale; |e'| <= dz -> centre; else t = clamp((|e'|-dz)/(span*dz), 0, 1) and the
//            colour is centre + (sharp|flat - centre)*t.  Medium: dz 0.0021, scale 0.7, span 4;
//            Hard: 0.00105, 0.9, 8; Relaxed: 0.0025, 0.7, 4.
//            Pure centre up to cents(dz/scale) = 6.17 (Relaxed), 5.19 (Medium), 2.02 (Hard);
//            saturated from cents(dz*(1+span)/scale) = 30.64, 25.78, 18.08.
//   history  duration-weighted mean of errRatio over the last H ms of voiced time (the segment on
//            the window edge enters fractionally), converted back to cents with 1200*log2(1+e).
//            ToneMap averages in the RATIO domain, so e.g. (1 s at +10 c, 2 s at -10 c) gives
//            -3.3076 c, not the cents mean -3.3333 c.
//   fade     hold h, then a linear 10 s ramp from the shown value d to 0.25*base:
//            d(el) = d for el <= h; d + (0.25*base - d)*(el-h)/10000 inside the ramp; 0.25*base after.
//   Relaxed  display EMA on every stained frame: d += (base - d)*(1 - exp(-dt/125)),
//            base = min(1, total/500)  (dt = the tick's dt: documented deviation, design section 0).
//   rebase   a history captured against A=a0 is read against a1 as (1+e)*(a0/a1) - 1,
//            e.g. 442 -> 440: 1200*log2(442/440) = +7.8514 c.
//   Tolerances: errors are stored as round(e*65536) (0.0131 c per LSB), so cents comparisons use
//            0.05 c; colours use +-1 per channel.
namespace mt {

using namespace tonemap;

Model gModel;  // the only Model of the bench: ~38 KB in .bss
uint32_t gNow = 100000;
uint32_t gTicks = 0;
int gFailures = 0;

struct Series {
    const char* name;
    int n = 0, bad = 0;
    char first[150] = "";
    explicit Series(const char* nm) : name(nm) {}
    void check(bool ok, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        ++n;
        if (ok) return;
        ++bad;
        if (first[0] != 0) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(first, sizeof(first), fmt, ap);
        va_end(ap);
    }
    void done(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char detail[150];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
        if (bad == 0) {
            rep("[test] %s: PASS (%d checks; %s)", name, n, detail);
        } else {
            ++gFailures;
            rep("[test] %s: FAIL (%d of %d checks failed; first: %s | %s)", name, bad, n, first, detail);
        }
    }
};

double ratioOf(double cents) { return pow(2.0, cents / 1200.0) - 1.0; }
double centsOf(double ratio) { return 1200.0 * log2(1.0 + ratio); }
double hzFor(int concertMidi, double cents, double a4) { return a4 * pow(2.0, (concertMidi - 69) / 12.0 + cents / 1200.0); }
double rnd(double x) { return x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5); }  // round half away from zero

TickResult frame(int concert, double cents, uint32_t dt, double a4) {
    gNow += dt;
    ++gTicks;
    return gModel.tick(true, (float)hzFor(concert, cents, a4), dt, gNow);
}

// nSeg segments of one note. The first valid frame of a note makes no segment, so one extra
// (priming) frame is sent when that note is not already active.
void feedNote(int concert, double cents, int nSeg, uint32_t dt, double a4 = 440.0) {
    if (gModel.activeConcertMidi() != concert) frame(concert, cents, dt, a4);
    for (int i = 0; i < nSeg; ++i) frame(concert, cents, dt, a4);
}

void gap(uint32_t dt) {
    gNow += dt;
    ++gTicks;
    gModel.tick(false, 0.0f, dt, gNow);
}

void fresh(Sensitivity s, int hist, int hold, float a4 = 440.0f, int transp = 0) {
    gNow += 1000;
    gModel.reset(gNow);
    gModel.setSensitivity(s, gNow);
    gModel.setHistory(hist, gNow);
    gModel.setHold(hold, gNow);
    gModel.setRefA(a4, gNow);
    gModel.setTransposition(transp, gNow);
}

bool rgbNear(const Rgb& c, int r, int g, int b, int tol) {
    return abs((int)c.r - r) <= tol && abs((int)c.g - g) <= tol && abs((int)c.b - b) <= tol;
}

// ---- parity ------------------------------------------------------------------------------
void testParity() {
    {
        Series s("parity equal/none");
        float maxDc = 0, maxDn = 0;
        int ok = 0;
        for (int i = 0; i < tmvec::kParityFixtureCount; ++i) {
            const tmvec::ParityRow& r = tmvec::kParityFixture[i];
            const NoteHit h = freqToNote(r.hz, r.a4);
            const float dc = fabsf(h.cents - r.cents);
            const float dn = fabsf(needlePct(h.cents) - r.needle);
            const bool pass = h.concertMidi == r.midi && dc <= 0.01f && dn <= 0.001f;
            if (h.concertMidi == r.midi) {
                if (dc > maxDc) maxDc = dc;
                if (dn > maxDn) maxDn = dn;
            }
            if (pass) ++ok;
            s.check(pass, "row %d hz=%.6f a4=%.0f midi %d/%d cents %.5f/%.5f needle %.5f/%.5f", i, (double)r.hz, (double)r.a4,
                    h.concertMidi, (int)r.midi, (double)h.cents, (double)r.cents, (double)needlePct(h.cents), (double)r.needle);
        }
        s.done("%d/%d rows, max|dcents|=%.5f max|dneedle|=%.6f", ok, tmvec::kParityFixtureCount, (double)maxDc, (double)maxDn);
    }
    {
        // Rows with transposition go through Model::tick (concert-pitch path); the expected values were
        // computed by the generator with ToneMap's adjusted-pitch path.
        Series s("parity extra");
        float maxDc = 0, maxDn = 0;
        int ok = 0;
        for (int i = 0; i < tmvec::kParityExtraCount; ++i) {
            const tmvec::ParityRow& r = tmvec::kParityExtra[i];
            gNow += 10;
            gModel.setRefA(r.a4, gNow);
            gModel.setTransposition(r.transp, gNow);
            const TickResult tr = gModel.tick(true, r.hz, 0, gNow);
            ++gTicks;
            const float dc = fabsf(tr.cents - r.cents);
            const float dn = fabsf(needlePct(tr.cents) - r.needle);
            bool pass = tr.hasNote && tr.writtenMidi == r.midi && dc <= 0.01f && dn <= 0.001f;
            if (r.transp == 0) {  // the pure function must agree as well
                const NoteHit h = freqToNote(r.hz, r.a4);
                pass = pass && h.concertMidi == r.midi && fabsf(h.cents - r.cents) <= 0.01f;
            }
            if (tr.hasNote && tr.writtenMidi == r.midi) {
                if (dc > maxDc) maxDc = dc;
                if (dn > maxDn) maxDn = dn;
            }
            if (pass) ++ok;
            s.check(pass, "row %d hz=%.5f a4=%.0f t=%d midi %d/%d cents %.4f/%.4f", i, (double)r.hz, (double)r.a4, (int)r.transp,
                    tr.writtenMidi, (int)r.midi, (double)tr.cents, (double)r.cents);
        }
        gModel.setRefA(440.0f, gNow);
        gModel.setTransposition(0, gNow);
        gModel.endNote();
        s.done("%d/%d rows, max|dcents|=%.5f max|dneedle|=%.6f", ok, tmvec::kParityExtraCount, (double)maxDc, (double)maxDn);
    }
    {
        Series s("needle position");
        for (int i = 0; i < tmvec::kNeedleRowCount; ++i) {
            const tmvec::NeedleRow& r = tmvec::kNeedleRows[i];
            const float p = needlePct(r.cents);
            s.check(fabsf(p - r.pct) <= 1e-6f, "cents %.4f pct %.6f expected %.6f", (double)r.cents, (double)p, (double)r.pct);
        }
        s.check(needlePct(NAN) == 0.5f, "NaN cents -> centre");
        s.done("%d rows incl. clamp at +-50 / +-60", tmvec::kNeedleRowCount);
    }
}

// ---- colour ------------------------------------------------------------------------------
struct ColorCase {
    Sensitivity s;
    float cents;
    uint8_t r, g, b;
};
// Expected from the bias formulas above (Python, double precision), display intensity 1.
const ColorCase kColorCases[] = {
    {Sensitivity::Medium, 0.0f, 34, 197, 94},      {Sensitivity::Medium, 5.0f, 34, 197, 94},
    {Sensitivity::Medium, 5.4f, 36, 197, 93},      {Sensitivity::Medium, 10.0f, 85, 191, 72},
    {Sensitivity::Medium, 15.6f, 145, 183, 47},    {Sensitivity::Medium, 20.0f, 193, 178, 26},
    {Sensitivity::Medium, 26.0f, 255, 170, 0},     {Sensitivity::Medium, 40.0f, 255, 170, 0},
    {Sensitivity::Medium, -5.0f, 34, 197, 94},     {Sensitivity::Medium, -5.4f, 34, 196, 95},
    {Sensitivity::Medium, -15.6f, 17, 142, 154},   {Sensitivity::Medium, -40.0f, 0, 86, 214},
    {Sensitivity::Hard, 0.0f, 34, 197, 94},        {Sensitivity::Hard, 1.9f, 34, 197, 94},
    {Sensitivity::Hard, 2.3f, 38, 197, 92},        {Sensitivity::Hard, 10.0f, 144, 184, 47},
    {Sensitivity::Hard, 18.2f, 255, 170, 0},       {Sensitivity::Hard, 25.0f, 255, 170, 0},
    {Sensitivity::Hard, -10.0f, 17, 142, 153},     {Sensitivity::Hard, -25.0f, 0, 86, 214},
    {Sensitivity::Relaxed, 0.0f, 34, 197, 94},     {Sensitivity::Relaxed, 6.0f, 34, 197, 94},
    {Sensitivity::Relaxed, 6.5f, 37, 197, 93},     {Sensitivity::Relaxed, 18.0f, 140, 184, 49},
    {Sensitivity::Relaxed, 31.0f, 255, 170, 0},    {Sensitivity::Relaxed, 40.0f, 255, 170, 0},
    {Sensitivity::Relaxed, -18.0f, 18, 144, 151},  {Sensitivity::Relaxed, -40.0f, 0, 86, 214},
};
// Same notes with only 250 ms of voiced time: base = display = 0.5, colour = round(bias*0.5).
const ColorCase kBlendCases[] = {
    {Sensitivity::Medium, 15.6f, 73, 92, 23},
    {Sensitivity::Medium, -15.6f, 9, 71, 77},
    {Sensitivity::Hard, 10.0f, 72, 92, 24},
    {Sensitivity::Medium, 0.0f, 17, 99, 47},
};
const char* sensName(Sensitivity s) { return s == Sensitivity::Relaxed ? "Relaxed" : (s == Sensitivity::Medium ? "Medium" : "Hard"); }

void testColor() {
    Series s("color");
    for (const ColorCase& c : kColorCases) {
        fresh(c.s, 1, 5);  // 3 s history, no fade
        feedNote(69, c.cents, 200, 20);  // 4 s: base 1, Relaxed display converged (1-e^-32)
        bool visited = false;
        const Rgb got = gModel.cellColor(69, gNow, &visited);
        s.check(rgbNear(got, c.r, c.g, c.b, 1) && visited, "%s %+.1fc got (%d,%d,%d) expected (%d,%d,%d) visited=%d", sensName(c.s),
                (double)c.cents, got.r, got.g, got.b, c.r, c.g, c.b, (int)visited);
    }
    for (const ColorCase& c : kBlendCases) {
        fresh(c.s, 1, 5);
        feedNote(69, c.cents, 10, 25);  // 250 ms voiced
        const Rgb got = gModel.cellColor(69, gNow, nullptr);
        const float disp = gModel.displayIntensity(69, gNow);
        s.check(rgbNear(got, c.r, c.g, c.b, 1) && fabsf(disp - 0.5f) < 0.001f, "blend %s %+.1fc got (%d,%d,%d) expected (%d,%d,%d) display %.4f",
                sensName(c.s), (double)c.cents, got.r, got.g, got.b, c.r, c.g, c.b, (double)disp);
    }
    // never-touched cell: black, not visited
    bool visited = true;
    const Rgb none = gModel.cellColor(70, gNow, &visited);
    s.check(none.r == 0 && none.g == 0 && none.b == 0 && !visited, "untouched cell black / unvisited");
    // direct API
    s.check(rgbNear(biasColor(Sensitivity::Medium, 0.0f), 34, 197, 94, 0), "biasColor centre");
    s.check(rgbNear(biasColor(Sensitivity::Medium, 0.05f), 255, 170, 0, 0), "biasColor sharp");
    s.check(rgbNear(biasColor(Sensitivity::Hard, -0.05f), 0, 86, 214, 0), "biasColor flat");
    s.check(rgbNear(biasColor(Sensitivity::Relaxed, NAN), 34, 197, 94, 0), "biasColor NaN -> centre");
    s.check(inDeadzone(Sensitivity::Medium, 0.0020f) && !inDeadzone(Sensitivity::Medium, 0.0022f), "deadzone Medium 0.21%%");
    s.check(inDeadzone(Sensitivity::Hard, 0.0010f) && !inDeadzone(Sensitivity::Hard, 0.0011f), "deadzone Hard 0.105%%");
    s.check(inDeadzone(Sensitivity::Relaxed, 0.0024f) && !inDeadzone(Sensitivity::Relaxed, 0.0026f), "deadzone Relaxed 0.25%%");
    s.check(!inDeadzone(Sensitivity::Medium, NAN), "deadzone NaN");
    s.check(baseIntensity(0) == 0.0f && fabsf(baseIntensity(250) - 0.5f) < 1e-6f && baseIntensity(500) == 1.0f && baseIntensity(99999) == 1.0f,
            "baseIntensity min(1, total/500)");
    s.done("%u colour cases +-1, %u blend cases at display 0.5", (unsigned)(sizeof(kColorCases) / sizeof(kColorCases[0])),
           (unsigned)(sizeof(kBlendCases) / sizeof(kBlendCases[0])));
}

// ---- needle smoothing / tick flags -----------------------------------------------------------
void testNeedle() {
    Series s("needle/tick flags");
    // Relaxed: EMA with tau 350 ms, seeded with the raw cents; reset on note change / invalid frame.
    fresh(Sensitivity::Relaxed, 1, 5);
    TickResult r = frame(69, 10.0, 32, 440.0);
    s.check(r.hasNote && r.newNote && r.concertMidi == 69 && r.writtenMidi == 69, "first frame flags");
    s.check(fabsf(r.displayCents - 10.0f) < 0.01f, "Relaxed seed %.3f", (double)r.displayCents);
    r = frame(69, 0.0, 350, 440.0);  // alpha = 1 - e^-1 = 0.63212: 10 + (0-10)*0.63212 = 3.6788
    s.check(!r.newNote && fabsf(r.displayCents - 3.6788f) < 0.01f && fabsf(r.cents) < 0.01f, "Relaxed EMA %.4f (expected 3.6788), raw %.4f",
            (double)r.displayCents, (double)r.cents);
    r = frame(69, 0.0, 0, 440.0);  // dt = 0: alpha = 0, the needle does not move
    s.check(fabsf(r.displayCents - 3.6788f) < 0.01f, "Relaxed dt=0 holds %.4f", (double)r.displayCents);
    r = frame(71, -20.0, 32, 440.0);  // note change: reseed with raw cents
    s.check(r.newNote && fabsf(r.displayCents + 20.0f) < 0.01f, "Relaxed reseed on note change %.3f", (double)r.displayCents);
    r = frame(71, -10.0, 100, 440.0);  // alpha = 1-e^(-100/350) = 0.24865: -20 + 10*0.24865 = -17.5135
    s.check(fabsf(r.displayCents + 17.5135f) < 0.01f, "Relaxed EMA 2 %.4f (expected -17.5135)", (double)r.displayCents);
    gap(32);
    r = frame(71, 5.0, 100, 440.0);  // invalid frame ended the note: reseed, new note
    s.check(r.newNote && fabsf(r.displayCents - 5.0f) < 0.01f, "Relaxed reseed after invalid frame %.3f", (double)r.displayCents);
    // Medium and Hard: raw cents (brief 3.6)
    const Sensitivity modes[2] = {Sensitivity::Medium, Sensitivity::Hard};
    for (int i = 0; i < 2; ++i) {
        fresh(modes[i], 1, 5);
        frame(69, 10.0, 32, 440.0);
        r = frame(69, -3.0, 80, 440.0);
        s.check(fabsf(r.displayCents - r.cents) < 1e-4f && fabsf(r.cents + 3.0f) < 0.01f, "%s needle is raw: %.4f vs %.4f", sensName(modes[i]),
                (double)r.displayCents, (double)r.cents);
    }
    // inDeadzone from the raw errRatio of the frame
    fresh(Sensitivity::Medium, 1, 5);
    r = frame(69, 3.0, 32, 440.0);  // ratio 0.00174 <= 0.0021
    s.check(r.inDeadzone, "Medium +3c in deadzone");
    r = frame(69, 4.0, 32, 440.0);  // ratio 0.00232 > 0.0021
    s.check(!r.inDeadzone, "Medium +4c outside");
    fresh(Sensitivity::Hard, 1, 5);
    r = frame(69, 1.5, 32, 440.0);  // 0.00087 <= 0.00105
    s.check(r.inDeadzone, "Hard +1.5c in deadzone");
    r = frame(69, 2.0, 32, 440.0);  // 0.00116 > 0.00105
    s.check(!r.inDeadzone, "Hard +2c outside");
    fresh(Sensitivity::Relaxed, 1, 5);
    r = frame(69, -4.0, 32, 440.0);  // 0.0023 <= 0.0025
    s.check(r.inDeadzone, "Relaxed -4c in deadzone");
    r = frame(69, -4.6, 32, 440.0);  // 0.00266 > 0.0025
    s.check(!r.inDeadzone, "Relaxed -4.6c outside");
    s.done("Relaxed EMA tau 350 ms, Medium / Hard raw, deadzone flags");
}

// ---- history ------------------------------------------------------------------------------
void testHistory() {
    Series s("history");
    const double rp = ratioOf(10.0), rm = ratioOf(-10.0);
    char detail[150];
    char* p = detail;
    int left = (int)sizeof(detail);
    {
        // A: 1 s at +10 c, then 2 s at -10 c (20 ms frames, one priming frame)
        fresh(Sensitivity::Medium, 1, 5);
        feedNote(69, 10.0, 50, 20);
        feedNote(69, -10.0, 100, 20);
        s.check(gModel.totalMs(69) == 3000, "A total %u", (unsigned)gModel.totalMs(69));
        const double want[4] = {rm, (1000.0 * rp + 2000.0 * rm) / 3000.0, (1000.0 * rp + 2000.0 * rm) / 3000.0, (1000.0 * rp + 2000.0 * rm) / 3000.0};
        const char* nm[4] = {"1s", "3s", "30s", "unl"};
        for (int i = 0; i < 4; ++i) {
            gModel.setHistory(i, gNow);
            const double got = centsOf(gModel.avgErrRatio(69));
            s.check(fabs(got - centsOf(want[i])) <= 0.05, "A H=%s got %.4f c expected %.4f c", nm[i], got, centsOf(want[i]));
            const int w = snprintf(p, left, "A%s=%+.2f ", nm[i], got);
            if (w > 0 && w < left) { p += w; left -= w; }
        }
        // Unlimited is exact: compare with the quantised integer sums (round(e*65536) per frame).
        const double qa = rnd(rp * 65536.0), qb = rnd(rm * 65536.0);
        const double exact = (1000.0 * qa + 2000.0 * qb) / 3000.0 / 65536.0;
        s.check(fabs((double)gModel.avgErrRatio(69) - exact) <= 2e-6, "A unlimited exact: %.8f vs %.8f", (double)gModel.avgErrRatio(69), exact);
    }
    {
        // B: 35 s at +10 c, then 5 s at -10 c (exercises the fine -> coarse cascade and ring eviction)
        fresh(Sensitivity::Medium, 1, 5);
        feedNote(69, 10.0, 1750, 20);
        feedNote(69, -10.0, 250, 20);
        s.check(gModel.totalMs(69) == 40000, "B total %u", (unsigned)gModel.totalMs(69));
        const NoteState* ns = gModel.note(69);
        s.check(ns && ns->fineCnt == kFineCap && ns->coarseCnt == kCoarseCap, "B rings full (fine %d coarse %d)", ns ? ns->fineCnt : -1,
                ns ? ns->coarseCnt : -1);
        const double want[4] = {rm, rm, (25000.0 * rp + 5000.0 * rm) / 30000.0, (35000.0 * rp + 5000.0 * rm) / 40000.0};
        const char* nm[4] = {"1s", "3s", "30s", "unl"};
        for (int i = 0; i < 4; ++i) {
            gModel.setHistory(i, gNow);
            const double got = centsOf(gModel.avgErrRatio(69));
            s.check(fabs(got - centsOf(want[i])) <= 0.05, "B H=%s got %.4f c expected %.4f c", nm[i], got, centsOf(want[i]));
            const int w = snprintf(p, left, "B%s=%+.2f ", nm[i], got);
            if (w > 0 && w < left) { p += w; left -= w; }
        }
        const double qa = rnd(rp * 65536.0), qb = rnd(rm * 65536.0);
        const double exact = (35000.0 * qa + 5000.0 * qb) / 40000.0 / 65536.0;
        s.check(fabs((double)gModel.avgErrRatio(69) - exact) <= 2e-6, "B unlimited exact: %.8f vs %.8f", (double)gModel.avgErrRatio(69), exact);
    }
    s.check(gModel.avgErrRatio(70) == 0.0f, "empty note average is 0");
    s.done("cents: %s", detail);
}

// ---- fade ---------------------------------------------------------------------------------
double fadeExpect(double d, double base, double holdMs, double el) {
    if (el <= holdMs) return d;
    const double target = 0.25 * base;
    if (el - holdMs >= 10000.0) return d < target ? d : target;
    return d + (target - d) * (el - holdMs) / 10000.0;
}

void testFade() {
    Series s("fade");
    // Medium, hold 5 s, base = 1: 1.0 up to the hold, linear ramp over 10 s to 0.25, then stays.
    fresh(Sensitivity::Medium, 1, 0);
    feedNote(69, 5.0, 25, 20);  // 500 ms of voice -> base 1
    const uint32_t t0 = gNow;
    const uint32_t offs[] = {0, 4000, 5000, 7500, 10000, 15000, 20000, 200000};
    for (uint32_t o : offs) {
        const double e = fadeExpect(1.0, 1.0, 5000.0, (double)o);
        const float g = gModel.displayIntensity(69, t0 + o);
        s.check(fabs((double)g - e) < 1e-4, "hold 5s +%us got %.4f expected %.4f", (unsigned)o, (double)g, e);
    }
    {
        bool v = false;
        const Rgb c = gModel.cellColor(69, t0 + 20000, &v);  // colour of a faded cell: centre*0.25
        s.check(rgbNear(c, 9, 49, 24, 1) && v, "faded colour (%d,%d,%d) expected (9,49,24)", c.r, c.g, c.b);  // round(0.25*(34,197,94)) = (9,49,24)
    }
    // base 0.5 (250 ms voiced): target 0.125
    fresh(Sensitivity::Medium, 1, 0);
    feedNote(69, 5.0, 10, 25);
    const uint32_t t1 = gNow;
    for (uint32_t o : {uint32_t(4000), uint32_t(10000), uint32_t(20000)}) {
        const double e = fadeExpect(0.5, 0.5, 5000.0, (double)o);
        const float g = gModel.displayIntensity(69, t1 + o);
        s.check(fabs((double)g - e) < 1e-4, "base .5 hold 5s +%us got %.4f expected %.4f", (unsigned)o, (double)g, e);
    }
    // no fade: stays at 1.0 for ever; 2 min hold
    fresh(Sensitivity::Medium, 1, 5);
    feedNote(69, 5.0, 25, 20);
    s.check(fabsf(gModel.displayIntensity(69, gNow + 3600000u) - 1.0f) < 1e-6f, "no fade stays 1.0");
    s.check(!gModel.anyFading(gNow + 3600000u), "no fade: anyFading false");
    fresh(Sensitivity::Medium, 1, 3);
    feedNote(69, 5.0, 25, 20);
    s.check(fabsf(gModel.displayIntensity(69, gNow + 100000u) - 1.0f) < 1e-6f, "hold 2 min at +100 s");
    s.check(fabsf(gModel.displayIntensity(69, gNow + 125000u) - 0.625f) < 1e-4f, "hold 2 min at +125 s: %.4f", (double)gModel.displayIntensity(69, gNow + 125000u));
    // anyFading window (hold 5 s)
    fresh(Sensitivity::Medium, 1, 0);
    feedNote(69, 5.0, 25, 20);
    const uint32_t t2 = gNow;
    s.check(!gModel.anyFading(t2 + 4000), "anyFading before hold");
    s.check(gModel.anyFading(t2 + 6000), "anyFading in the ramp");
    s.check(gModel.anyFading(t2 + 15300), "anyFading just after the ramp (settle)");
    s.check(!gModel.anyFading(t2 + 16000), "anyFading after settle");
    // setHold does not restart the timer; the other setters do
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 12000) - 0.475) < 1e-4, "mid-ramp +12 s = 0.475 (%.4f)", (double)gModel.displayIntensity(69, t2 + 12000));
    gModel.setHold(2, t2 + 12000);  // 30 s: Medium display back to base at once, same stain time
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 12000) - 1.0) < 1e-4, "setHold: back to 1.0 (%.4f)", (double)gModel.displayIntensity(69, t2 + 12000));
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 35000) - fadeExpect(1.0, 1.0, 30000.0, 35000.0)) < 1e-4, "setHold: no restart, +35 s = 0.625 (%.4f)",
            (double)gModel.displayIntensity(69, t2 + 35000));
    gModel.setHold(0, t2 + 12000);
    gModel.setHistory(2, t2 + 12000);  // recompute: timers restart at +12 s
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 12000) - 1.0) < 1e-4, "setHistory: jumps to 1.0");
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 16000) - 1.0) < 1e-4, "setHistory: hold restarted (+4 s)");
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 22000) - 0.625) < 1e-4, "setHistory: ramp restarted (+10 s) %.4f", (double)gModel.displayIntensity(69, t2 + 22000));
    gModel.setRefA(440.0f, t2 + 30000);
    s.check(fabs((double)gModel.displayIntensity(69, t2 + 30000) - 1.0) < 1e-4, "setRefA restarts the timer");

    // Relaxed: display EMA towards base on stained frames only, alpha = 1 - exp(-dt/125).
    fresh(Sensitivity::Relaxed, 1, 0);
    frame(69, 5.0, 20, 440.0);  // priming frame, no segment
    double d = 0.0;
    const double alpha = 1.0 - exp(-20.0 / 125.0);
    char detail[100] = "";
    for (int k = 1; k <= 25; ++k) {
        frame(69, 5.0, 20, 440.0);
        const double base = fmin(1.0, 20.0 * k / 500.0);
        d += (base - d) * alpha;
        if (k == 1 || k == 5 || k == 12 || k == 25) {
            const float g = gModel.displayIntensity(69, gNow);
            s.check(fabs((double)g - d) < 2e-4, "Relaxed EMA n=%d got %.5f expected %.5f", k, (double)g, d);
            if (k == 25) snprintf(detail, sizeof(detail), "Relaxed display after 25 ticks %.4f", d);
        }
    }
    const uint32_t t3 = gNow;
    // fade of the stored (EMA) value with hold 5 s
    s.check(fabs((double)gModel.displayIntensity(69, t3 + 10000) - fadeExpect(d, 1.0, 5000.0, 10000.0)) < 2e-4, "Relaxed fade +10 s %.4f",
            (double)gModel.displayIntensity(69, t3 + 10000));
    s.check(fabs((double)gModel.displayIntensity(69, t3 + 20000) - 0.25) < 2e-4, "Relaxed faded to 0.25");
    // a new stain after the fade starts from the faded value (materialised): 0.25 + 0.75*alpha
    gNow = t3 + 20000;
    gModel.tick(true, (float)hzFor(69, 5.0, 440.0), 20, gNow);
    ++gTicks;
    const double e2 = 0.25 + (1.0 - 0.25) * alpha;
    s.check(fabs((double)gModel.displayIntensity(69, gNow) - e2) < 2e-4, "Relaxed restain from 0.25: got %.5f expected %.5f", (double)gModel.displayIntensity(69, gNow), e2);
    // Medium / Hard have no fade-in: display = base at the first stained frame
    fresh(Sensitivity::Hard, 1, 5);
    frame(69, 5.0, 25, 440.0);
    frame(69, 5.0, 25, 440.0);  // 25 ms voiced -> base 0.05
    s.check(fabs((double)gModel.displayIntensity(69, gNow) - 0.05) < 1e-4, "Hard display = base immediately (%.4f)", (double)gModel.displayIntensity(69, gNow));
    s.done("hold 5 s / 2 min / none, base 0.5, setHold vs recompute timers, anyFading, %s", detail);
}

// ---- re-basing of A4 ----------------------------------------------------------------------
void testRebase() {
    Series s("rebase");
    char detail[120];
    {
        fresh(Sensitivity::Medium, 1, 5, 442.0f);
        feedNote(69, 0.0, 100, 20, 442.0);  // played exactly on A=442
        const double c0 = centsOf(gModel.avgErrRatio(69));
        gModel.setRefA(440.0f, gNow);
        const double c1 = centsOf(gModel.avgErrRatio(69));
        gModel.setRefA(442.0f, gNow);
        const double c2 = centsOf(gModel.avgErrRatio(69));
        const double want = 1200.0 * log2(442.0 / 440.0);  // +7.8514
        s.check(fabs(c0) <= 0.02, "442 own pitch: %.4f c", c0);
        s.check(fabs(c1 - want) <= 0.02, "442 -> 440: %.4f c, expected +%.4f", c1, want);
        s.check(fabs(c2) <= 0.02, "back to 442: %.4f c", c2);
        snprintf(detail, sizeof(detail), "442->440 %+.3f c (expected %+.3f)", c1, want);
    }
    {
        fresh(Sensitivity::Medium, 1, 5, 440.0f);
        feedNote(69, 0.0, 100, 20, 440.0);
        gModel.setRefA(442.0f, gNow);
        const double c = centsOf(gModel.avgErrRatio(69));
        s.check(fabs(c + 7.8514) <= 0.02, "440 -> 442: %.4f c, expected -7.8514", c);
    }
    {
        // extremes: captured at 400, read at 480 and back
        fresh(Sensitivity::Medium, 1, 5, 400.0f);
        feedNote(69, 0.0, 100, 20, 400.0);
        gModel.setRefA(480.0f, gNow);
        const double c = centsOf(gModel.avgErrRatio(69));
        const double want = 1200.0 * log2(400.0 / 480.0);
        s.check(fabs(c - want) <= 0.05, "400 -> 480: %.4f c, expected %.4f", c, want);
        gModel.setRefA(400.0f, gNow);
        s.check(fabs(centsOf(gModel.avgErrRatio(69))) <= 0.05, "480 -> 400 back: %.4f c", centsOf(gModel.avgErrRatio(69)));
        fresh(Sensitivity::Medium, 1, 5, 480.0f);
        feedNote(69, 0.0, 100, 20, 480.0);
        gModel.setRefA(400.0f, gNow);
        const double c2 = centsOf(gModel.avgErrRatio(69));
        const double want2 = 1200.0 * log2(480.0 / 400.0);
        s.check(fabs(c2 - want2) <= 0.05, "480 -> 400: %.4f c, expected %.4f", c2, want2);
    }
    {
        // 160 Up / Down presses (440 -> 480 -> 400 -> 440): no drift, and every step matches the formula.
        fresh(Sensitivity::Medium, 1, 5, 440.0f);
        feedNote(69, 8.0, 100, 20, 440.0);
        const double f = 440.0 * pow(2.0, 8.0 / 1200.0);
        const float start = gModel.avgErrRatio(69);
        double worst = 0.0;
        int a = 440;
        const int steps[3] = {+1, -1, +1};
        const int counts[3] = {40, 80, 40};
        int presses = 0;
        for (int ph = 0; ph < 3; ++ph) {
            for (int i = 0; i < counts[ph]; ++i) {
                a += steps[ph];
                gModel.setRefA((float)a, gNow);
                ++presses;
                const double want = f / (double)a - 1.0;  // the same frequency scored against A=a
                const double dev = fabs((double)gModel.avgErrRatio(69) - want);
                if (dev > worst) worst = dev;
            }
        }
        s.check(worst <= 2e-5, "press-by-press max deviation %.2e (ratio)", worst);
        s.check(a == 440 && gModel.avgErrRatio(69) == start, "no drift after %d presses: %.9f vs %.9f", presses, (double)gModel.avgErrRatio(69), (double)start);
    }
    {
        // the re-basing is exact for a mixed history spread over fine and coarse chunks
        fresh(Sensitivity::Medium, 2, 5, 440.0f);
        feedNote(69, 12.0, 400, 32, 440.0);
        feedNote(69, -9.0, 400, 32, 440.0);
        const double before = (double)gModel.avgErrRatio(69);
        gModel.setRefA(444.0f, gNow);
        const double after = (double)gModel.avgErrRatio(69);
        const double want = (1.0 + before) * (440.0 / 444.0) - 1.0;
        s.check(fabs(after - want) <= 1e-6, "mixed history 440 -> 444: %.8f vs %.8f", after, want);
    }
    gModel.setRefA(440.0f, gNow);
    s.done("%s; 400/480 extremes; 160 presses without drift", detail);
}

// ---- transposition ------------------------------------------------------------------------
void testTranspose() {
    Series s("transposition");
    fresh(Sensitivity::Medium, 1, 5);
    feedNote(69, 15.6, 100, 20);  // concert A4, played 15.6 c sharp
    bool v = false;
    const Rgb c0 = gModel.cellColor(69, gNow, &v);
    s.check(rgbNear(c0, 145, 183, 47, 1) && v, "C: A4 cell (%d,%d,%d) expected (145,183,47)", c0.r, c0.g, c0.b);
    const int8_t semis[3] = {-2, 3, 5};
    const int written[3] = {71, 66, 64};  // Bb, Eb, F: written = concert - t
    const char* nm[3] = {"Bb", "Eb", "F"};
    for (int i = 0; i < 3; ++i) {
        gModel.setTransposition(semis[i], gNow);
        s.check(gModel.activeConcertMidi() == -1, "%s: setTransposition ends the active note", nm[i]);
        bool vis = false;
        const Rgb c = gModel.cellColor(written[i], gNow, &vis);
        s.check(rgbNear(c, 145, 183, 47, 1) && vis, "%s: concert A4 at written %d: (%d,%d,%d)", nm[i], written[i], c.r, c.g, c.b);
        bool vis69 = true;
        const Rgb old = gModel.cellColor(69, gNow, &vis69);
        s.check(old.r == 0 && old.g == 0 && old.b == 0 && !vis69, "%s: old cell 69 is empty", nm[i]);
        // a tick of concert A4 reports the written note
        const TickResult tr = gModel.tick(true, 440.0f, 0, gNow);
        s.check(tr.hasNote && tr.concertMidi == 69 && tr.writtenMidi == written[i], "%s: tick written %d concert %d", nm[i], tr.writtenMidi, tr.concertMidi);
        gModel.endNote();
        gModel.setTransposition(0, gNow);
        const Rgb back = gModel.cellColor(69, gNow, nullptr);
        s.check(back.r == c0.r && back.g == c0.g && back.b == c0.b, "%s -> C restores the colour", nm[i]);
    }
    // C -> F -> C in one go
    gModel.setTransposition(5, gNow);
    gModel.setTransposition(0, gNow);
    const Rgb again = gModel.cellColor(69, gNow, nullptr);
    s.check(again.r == c0.r && again.g == c0.g && again.b == c0.b, "C -> F -> C restores");
    // out of range written notes are black, never crash
    const Rgb o1 = gModel.cellColor(-30, gNow, &v);
    s.check(o1.r == 0 && !v, "written -30 black");
    const Rgb o2 = gModel.cellColor(500, gNow, &v);
    s.check(o2.r == 0 && !v, "written 500 black");
    gModel.setTransposition(-2, gNow);
    const Rgb o3 = gModel.cellColor(127, gNow, &v);  // concert 125: empty
    s.check(o3.r == 0, "written 127 under Bb");
    gModel.setTransposition(0, gNow);
    s.done("A4 -> written 71 (Bb), 66 (Eb), 64 (F); C->F->C restores");
}

// ---- chunk equivalence vs brute force ---------------------------------------------------------
struct Rng {
    uint32_t st;
    uint32_t next() {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        return st;
    }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};
constexpr int kRefMax = 4096;
uint16_t refDt[kRefMax];  // bench-only brute-force reference: every segment of the stream
int16_t refQ[kRefMax];

// ToneMap getRecentWeightedErr over the reference segments (double), then re-based to a4.
double refAvgRatio(int n, uint32_t H, double a4) {
    double wsum = 0.0, tot = 0.0, rem = (double)H;
    for (int i = n - 1; i >= 0; --i) {
        if (H != 0 && rem <= 0.0) break;
        const double take = (H == 0) ? (double)refDt[i] : fmin((double)refDt[i], rem);
        wsum += take * (double)refQ[i];
        tot += take;
        rem -= take;
    }
    if (tot <= 0.0) return 0.0;
    const double stored = wsum / tot / 65536.0;
    return (1.0 + stored) * (440.0 / a4) - 1.0;
}

// 120 s of frames at A=442 (concert A4). The stream is built from integer stored errors q (so the
// reference and the model quantise identically): hz = 442*(1+e), e = (1+q/65536)*(440/442) - 1.
// aligned: dt 32 ms and one constant q per 33 segments (= one coarse chunk of 11 fine chunks of 3
//   frames), so every chunk is uniform and the model must equal the brute force within float noise
//   for any H.
// random: dt 32 or 64 ms, a new q every frame, 0.5% invalid frames; the only allowed difference is
//   the fractional edge chunk: (largest chunk / H) * (q range) plus float noise.
void chunkStream(bool aligned, uint32_t seed, Series& s, double dev[4], int& checkpoints) {
    Rng rng = {seed};
    const double a4 = 442.0;
    fresh(Sensitivity::Medium, 1, 5, 442.0f);
    int n = 0;
    bool prevValid = false;
    uint32_t tms = 0;
    int validFrames = 0, group = -1;
    int q = 0, qmin = 1 << 20, qmax = -(1 << 20);
    const uint32_t H[4] = {1000, 3000, 30000, 0};
    for (int i = 0; i < 4; ++i) dev[i] = 0.0;
    checkpoints = 0;
    while (tms < 120000 && n < kRefMax - 1) {
        uint32_t dt = 32;
        bool valid = true;
        if (aligned) {
            if (n / 33 != group) {
                group = n / 33;
                q = rng.range(-1200, 1200);
            }
        } else {
            if (rng.next() % 10 == 0) dt = 64;
            if (rng.next() % 200 == 0) valid = false;
            q = rng.range(-1200, 1200);
        }
        gNow += dt;
        tms += dt;
        ++gTicks;
        if (!valid) {
            gModel.tick(false, 0.0f, dt, gNow);
            prevValid = false;
            continue;
        }
        const double e = (1.0 + q / 65536.0) * (440.0 / a4) - 1.0;
        gModel.tick(true, (float)(a4 * (1.0 + e)), dt, gNow);
        if (prevValid) {
            refDt[n] = (uint16_t)dt;
            refQ[n] = (int16_t)q;
            ++n;
            if (q < qmin) qmin = q;
            if (q > qmax) qmax = q;
        }
        prevValid = true;
        if (++validFrames % 62 == 0 && n > 0) {
            ++checkpoints;
            for (int k = 0; k < 4; ++k) {
                gModel.setHistory(k, gNow);
                const double got = (double)gModel.avgErrRatio(69);
                const double want = refAvgRatio(n, H[k], a4);
                const double d = fabs(got - want);
                if (d > dev[k]) dev[k] = d;
                double bound;
                if (H[k] == 0) bound = 2e-6;
                else if (aligned) bound = 2e-6;
                else bound = ((H[k] <= 3000 ? 170.0 : 1300.0) / (double)H[k]) * ((qmax - qmin) / 65536.0) + 2e-6;
                s.check(d <= bound, "%s seed %u t=%u H=%u got %.8f want %.8f bound %.2e", aligned ? "aligned" : "random", (unsigned)seed,
                        (unsigned)tms, (unsigned)H[k], got, want, bound);
            }
        }
    }
    uint32_t refTotal = 0;
    for (int i = 0; i < n; ++i) refTotal += refDt[i];
    s.check(gModel.totalMs(69) == refTotal, "%s total %u vs reference %u", aligned ? "aligned" : "random", (unsigned)gModel.totalMs(69), (unsigned)refTotal);
    const NoteState* ns = gModel.note(69);
    s.check(ns && ns->fineCnt == kFineCap && ns->coarseCnt == kCoarseCap, "%s rings full after 120 s", aligned ? "aligned" : "random");
    // Unlimited is exact to float noise in both modes
    gModel.setHistory(3, gNow);
    s.check(fabs((double)gModel.avgErrRatio(69) - refAvgRatio(n, 0, a4)) <= 2e-6, "%s unlimited exact", aligned ? "aligned" : "random");
}

void testChunks() {
    Series s("chunk equivalence");
    double devA[4], devR[4];
    int cpA = 0, cpR = 0;
    chunkStream(true, 0x9E3779B9u, s, devA, cpA);
    chunkStream(false, 0xC0FFEE11u, s, devR, cpR);
    s.done("aligned max dev (1s/3s/30s/unl) %.1e %.1e %.1e %.1e; random %.1e %.1e %.1e %.1e; %d+%d checkpoints, 3 refs of ~3700 segments",
           devA[0], devA[1], devA[2], devA[3], devR[0], devR[1], devR[2], devR[3], cpA, cpR);
}

// ---- first-frame rule ----------------------------------------------------------------------
void testFirstFrame() {
    Series s("first-frame rule");
    fresh(Sensitivity::Medium, 1, 5);
    TickResult r = frame(69, 0.0, 32, 440.0);
    s.check(r.hasNote && r.newNote && gModel.totalMs(69) == 0, "1 frame -> 0 segments (total %u)", (unsigned)gModel.totalMs(69));
    fresh(Sensitivity::Medium, 1, 5);
    for (int i = 0; i < 5; ++i) {
        r = frame(69, 0.0, 32, 440.0);
        s.check(r.newNote == (i == 0), "newNote flag at frame %d", i);
    }
    s.check(gModel.totalMs(69) == 4 * 32, "5 frames -> 4 segments (total %u, expected 128)", (unsigned)gModel.totalMs(69));
    // invalid frame ends the note: 3 valid, 1 invalid, 3 valid -> 2 + 2 segments
    fresh(Sensitivity::Medium, 1, 5);
    for (int i = 0; i < 3; ++i) frame(69, 0.0, 32, 440.0);
    gap(32);
    s.check(gModel.activeConcertMidi() == -1, "invalid frame clears the active note");
    for (int i = 0; i < 3; ++i) frame(69, 0.0, 32, 440.0);
    s.check(gModel.totalMs(69) == 4 * 32, "3+gap+3 -> 4 segments (total %u)", (unsigned)gModel.totalMs(69));
    // note change: A,A,A,B,B,B,A,A,A -> A: 2+2 segments, B: 2
    fresh(Sensitivity::Medium, 1, 5);
    const int seq[9] = {69, 69, 69, 71, 71, 71, 69, 69, 69};
    for (int i = 0; i < 9; ++i) {
        r = frame(seq[i], 0.0, 32, 440.0);
        const bool expNew = (i == 0 || i == 3 || i == 6);
        s.check(r.newNote == expNew && r.concertMidi == seq[i], "A/B/A newNote at frame %d", i);
    }
    s.check(gModel.totalMs(69) == 4 * 32 && gModel.totalMs(71) == 2 * 32, "A/B/A totals %u / %u (expected 128 / 64)", (unsigned)gModel.totalMs(69),
            (unsigned)gModel.totalMs(71));
    // dtMs == 0: no segment, the note continues
    fresh(Sensitivity::Medium, 1, 5);
    for (int i = 0; i < 3; ++i) frame(69, 0.0, 32, 440.0);
    for (int i = 0; i < 3; ++i) {
        r = frame(69, 0.0, 0, 440.0);
        s.check(r.hasNote && !r.newNote, "dt=0 frame keeps the note");
    }
    s.check(gModel.totalMs(69) == 64, "dt=0 frames add nothing (total %u)", (unsigned)gModel.totalMs(69));
    // segment of the frame is credited to the note of that frame (B segment goes to B)
    fresh(Sensitivity::Medium, 1, 5);
    frame(69, 0.0, 32, 440.0);
    frame(71, 0.0, 32, 440.0);
    s.check(gModel.totalMs(69) == 0 && gModel.totalMs(71) == 0, "a note change makes no segment for either note");
    // endNote() (pause) makes the next frame a first frame
    frame(71, 0.0, 32, 440.0);
    gModel.endNote();
    frame(71, 0.0, 32, 440.0);
    s.check(gModel.totalMs(71) == 32, "endNote -> next frame is a first frame (total %u)", (unsigned)gModel.totalMs(71));
    s.done("n frames -> n-1 segments; invalid frame / note change / endNote restart; dt=0 adds nothing");
}

// ---- robustness ----------------------------------------------------------------------------
void testRobustness() {
    Series s("robustness");
    const float inf = INFINITY;
    const float bad[] = {0.0f, NAN, inf, -inf, -5.0f, -0.0f, 1e-30f, 3.0e38f, 1e30f, 5.0f, 100000.0f};
    fresh(Sensitivity::Medium, 1, 5);
    frame(69, 0.0, 32, 440.0);  // a live note, to see that bad input ends it
    for (float hz : bad) {
        frame(69, 0.0, 32, 440.0);
        gNow += 32;
        const TickResult t1 = gModel.tick(true, hz, 32, gNow);
        s.check(!t1.hasNote && gModel.activeConcertMidi() == -1 && t1.concertMidi == -1, "valid=true hz=%g must end the note", (double)hz);
        const TickResult t2 = gModel.tick(false, hz, 32, gNow);
        s.check(!t2.hasNote, "valid=false hz=%g", (double)hz);
        s.check(freqToNote(hz, 440.0f).concertMidi == -1, "freqToNote(%g) rejects", (double)hz);
    }
    for (int m = 0; m < kNoteCount; ++m) {
        if (m != 69) s.check(gModel.totalMs(m) == 0, "note %d untouched", m);
    }
    s.check(freqToNote(440.0f, NAN).concertMidi == -1 && freqToNote(440.0f, 0.0f).concertMidi == -1 && freqToNote(440.0f, -1.0f).concertMidi == -1,
            "freqToNote bad A4");
    // dt = 0 and a huge dt
    fresh(Sensitivity::Medium, 1, 5);
    frame(69, 0.0, 32, 440.0);
    frame(69, 0.0, 0, 440.0);
    s.check(gModel.totalMs(69) == 0, "dt=0 right after the first frame");
    gNow += 5;
    gModel.tick(true, 440.0f, 0xFFFFFFFFu, gNow);
    s.check(gModel.totalMs(69) == kMaxSegMs, "dt=0xFFFFFFFF is capped at %u (total %u)", (unsigned)kMaxSegMs, (unsigned)gModel.totalMs(69));
    // frames with the extreme stored error clamp (q16 saturates, no overflow)
    fresh(Sensitivity::Medium, 1, 5, 480.0f);
    frame(69, 49.0, 32, 480.0);
    for (int i = 0; i < 200; ++i) frame(69, 49.0, 2000, 480.0);
    s.check(fabsf(gModel.avgErrRatio(69) - (float)ratioOf(49.0)) < 3e-5f, "2000 ms frames at +49 c: %.6f vs %.6f", (double)gModel.avgErrRatio(69), ratioOf(49.0));
    // setters with silly arguments
    gModel.setRefA(NAN, gNow);
    gModel.setRefA(-3.0f, gNow);
    s.check(gModel.refA() == 480.0f, "setRefA ignores NaN / negative");
    gModel.setHistory(-5, gNow);
    gModel.setHistory(99, gNow);
    gModel.setHold(-1, gNow);
    gModel.setHold(99, gNow);
    gModel.setTransposition(100000, gNow);
    gModel.setTransposition(-100000, gNow);
    gModel.setTransposition(0, gNow);
    s.check(gModel.historyIdx() == 3 && gModel.holdIdx() == 5 && gModel.transposition() == 0, "setters clamp their index");
    // accessors with bad indices
    s.check(gModel.avgErrRatio(-1) == 0.0f && gModel.avgErrRatio(128) == 0.0f && gModel.totalMs(500) == 0 && gModel.displayIntensity(-3, gNow) == 0.0f &&
                gModel.note(-1) == nullptr && gModel.note(128) == nullptr,
            "accessors reject bad indices");
    // reset clears everything
    fresh(Sensitivity::Medium, 1, 0);
    feedNote(69, 10.0, 100, 20);
    feedNote(60, -5.0, 100, 20);
    gModel.reset(gNow);
    bool clean = gModel.activeConcertMidi() == -1 && !gModel.anyFading(gNow + 20000);
    for (int m = 0; m < kNoteCount; ++m) {
        bool v = true;
        const Rgb c = gModel.cellColor(m, gNow, &v);
        if (gModel.totalMs(m) != 0 || gModel.avgErrRatio(m) != 0.0f || gModel.displayIntensity(m, gNow) != 0.0f || c.r || c.g || c.b || v) clean = false;
    }
    s.check(clean, "reset clears every note, colour and flag");
    frame(69, 0.0, 32, 440.0);
    const TickResult r = frame(69, 0.0, 32, 440.0);
    s.check(r.hasNote && gModel.totalMs(69) == 32, "usable after reset");
    s.done("0/NaN/+-inf/negative/denormal/huge Hz with valid and invalid, dt=0, dt max, bad indices, reset");
}

// ---- budget --------------------------------------------------------------------------------
void testBudget() {
    Series s("budget");
    s.check(sizeof(NoteState) == 296, "sizeof(NoteState) = %u", (unsigned)sizeof(NoteState));
    s.check(sizeof(Model) <= 40960, "sizeof(Model) = %u", (unsigned)sizeof(Model));
    s.check(sizeof(Chunk) == 4, "sizeof(Chunk) = %u", (unsigned)sizeof(Chunk));
    s.done("sizeof(NoteState)=%u sizeof(Model)=%u (budget 40960)", (unsigned)sizeof(NoteState), (unsigned)sizeof(Model));
}

void runModelTests() {
    const uint32_t t0 = millis();
    gFailures = 0;
    gTicks = 0;
    testBudget();
    testParity();
    testColor();
    testNeedle();
    testHistory();
    testFade();
    testRebase();
    testTranspose();
    testChunks();
    testFirstFrame();
    testRobustness();
    fresh(Sensitivity::Medium, 1, 2);  // leave the model clean
    gModel.reset(gNow);
    rep("[test] SUMMARY: %s | FAILURES=%d", gFailures == 0 ? "ALL PASS" : "FAILED", gFailures);
    rep("[test] model test phase took %u ms (%u ticks)", (unsigned)(millis() - t0), (unsigned)gTicks);
}

}  // namespace mt

// ---------------------------------------------------------------------------------------
// Live microphone mode
// ---------------------------------------------------------------------------------------
M5Canvas canvas(&M5Cardputer.Display);
bool drawOn = false;
bool spriteOk = false;
uint32_t lastDrawMs = 0, frameNo = 0;
uint32_t secDrawUs = 0, secPushUs = 0, secDrawFrames = 0;

bool liveRunning = false;
uint32_t liveStartMs = 0;

// last analysis (for the printed line)
struct Last {
    bool valid = false;
    char note[8] = "--";
    float cents = 0, hz = 0, clar = 0, rms = 0;
} last;

// counters of the current second
uint32_t secStartMs = 0, secWindows = 0, secFull = 0, secAnUsSum = 0, secAnUsMax = 0;
uint32_t secLoops = 0, secLoopUsMax = 0;
uint32_t lastLoopUs = 0;
uint32_t secChunkN0 = 0, secChunkUs0 = 0;
// published values (last completed second)
float pubAnPerS = 0, pubFullPerS = 0, pubChunksPerS = 0, pubLoopMs = 0, pubLoopMaxMs = 0, pubAnMsAvg = 0, pubAnMsMax = 0,
      pubDrawMs = 0, pubPushMs = 0;

// 10 s clock measurement (window and cumulative since 2 s after the start)
uint32_t clkWinN0 = 0, clkWinUs0 = 0, clkWinStartMs = 0;
uint32_t clkCumN0 = 0, clkCumUs0 = 0;
bool clkCumValid = false;

uint32_t lastLinePrintMs = 0, lastReportMs = 0;

void analyse() {
    const uint32_t t0 = micros();
    const mpm::Levels lv = mpm::prepare(sig, kN, work);
    mpm::Result r = {false, 0, 0, 0};
    const bool gated = lv.rms < kGateRms;
    if (!gated) {
        r = mpm::findPitch(work, kN, tonemap_audio::effectiveRate(), kLiveMaxLag, nsdf);
        ++secFull;
    }
    const uint32_t us = micros() - t0;
    ++secWindows;
    secAnUsSum += us;
    if (us > secAnUsMax) secAnUsMax = us;

    last.rms = lv.rms;
    last.clar = r.found ? r.clarity : 0.0f;
    last.valid = !gated && r.found && r.clarity > kMinClarity && r.freq > 14.0f && r.freq < 8200.0f;
    if (last.valid) {
        const float semis = 12.0f * log2f(r.freq / 440.0f);
        const int midi = (int)lroundf(semis) + 69;
        last.cents = (semis - (float)(midi - 69)) * 100.0f;
        last.hz = r.freq;
        noteName(midi, last.note, sizeof(last.note));
    } else {
        last.cents = 0;
        last.hz = 0;
        strlcpy(last.note, "--", sizeof(last.note));
    }
}

void drawLoadStep() {
    if (!spriteOk) {
        canvas.setColorDepth(16);
        spriteOk = canvas.createSprite(240, 125) != nullptr;
        if (!spriteOk) {
            Serial.println("[load] sprite allocation FAILED, draw load disabled");
            drawOn = false;
            return;
        }
    }
    const uint32_t t0 = micros();
    canvas.fillSprite(BLACK);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(WHITE);
    char buf[40];
    snprintf(buf, sizeof(buf), "bench draw load %u", (unsigned)frameNo);
    canvas.drawString(buf, 2, 3);
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 12; ++col) {
            const int cell = row * 12 + col;
            const uint8_t g = (uint8_t)(((frameNo * 5 + cell * 9) & 63) * 4);
            const int x = col * 20, y = 17 + row * 18;
            canvas.fillRect(x, y, 20, 18, canvas.color565(g, (uint8_t)(g / 2), (uint8_t)(255 - g)));
            canvas.drawRect(x, y, 21, 19, canvas.color565(55, 65, 81));
            canvas.drawString(kSharpNames[col], x + 2, y + 2);
        }
    }
    const uint32_t t1 = micros();
    canvas.pushSprite(0, 0);
    M5Cardputer.Display.waitDisplay();
    const uint32_t t2 = micros();
    secDrawUs += t1 - t0;
    secPushUs += t2 - t1;
    ++secDrawFrames;
    ++frameNo;
}

void printHeader() {
    rep("[bench] ToneMap bench, build %s %s", __DATE__, __TIME__);
    rep("[bench] cpu=%uMHz heap_free=%u heap_max_block=%u psram=%u", (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getPsramSize());
    rep("[bench] sizeof(NoteState)=%u sizeof(Model)=%u budget=40960", (unsigned)sizeof(tonemap::NoteState),
        (unsigned)sizeof(tonemap::Model));
    rep("[bench] buffers: ring=%u win=%u work=%u nsdf=%u bytes; live fs=%u effective=%.1f maxLag=%d gate_rms=%.0f clar>%.2f",
        (unsigned)(tonemap_audio::kRingChunks * tonemap_audio::kHop * 2), (unsigned)(kN * 2), (unsigned)(kN * 4),
        (unsigned)((mpm::kMaxLag + 2) * 4), (unsigned)tonemap_audio::kSampleRate, tonemap_audio::effectiveRate(), kLiveMaxLag,
        kGateRms, kMinClarity);
    rep("[bench] keys: d=toggle draw load, r=reprint report, a=reprint synthetic cases");
}

void handleSerial() {
    while (Serial.available() > 0) {
        const int ch = Serial.read();
        if (ch == 'd' || ch == 'D') {
            drawOn = !drawOn;
            Serial.printf("[load] draw load %s (240x125 sprite every 50 ms)\n", drawOn ? "ON" : "OFF");
            if (!drawOn && spriteOk) {
                M5Cardputer.Display.fillScreen(BLACK);
            }
        } else if (ch == 'r' || ch == 'R') {
            reprintReport();
        } else if (ch == 'a' || ch == 'A') {
            reprintCases();
        }
    }
}

void secondTick(uint32_t now) {
    if (now - secStartMs < 1000) return;
    const float el = (float)(now - secStartMs) / 1000.0f;
    pubAnPerS = (float)secWindows / el;
    pubFullPerS = (float)secFull / el;
    pubLoopMs = secLoops ? (el * 1000.0f) / (float)secLoops : 0.0f;
    pubLoopMaxMs = (float)secLoopUsMax / 1000.0f;
    pubAnMsAvg = secWindows ? (float)secAnUsSum / (float)secWindows / 1000.0f : 0.0f;
    pubAnMsMax = (float)secAnUsMax / 1000.0f;
    pubDrawMs = secDrawFrames ? (float)secDrawUs / (float)secDrawFrames / 1000.0f : 0.0f;
    pubPushMs = secDrawFrames ? (float)secPushUs / (float)secDrawFrames / 1000.0f : 0.0f;

    uint32_t n, us;
    tonemap_audio::chunkClock(n, us);
    const uint32_t dUs = us - secChunkUs0;
    pubChunksPerS = (dUs > 0 && n != secChunkN0) ? (float)(n - secChunkN0) * 1e6f / (float)dUs : 0.0f;
    secChunkN0 = n;
    secChunkUs0 = us;

    secStartMs = now;
    secWindows = secFull = secAnUsSum = secAnUsMax = 0;
    secLoops = secLoopUsMax = 0;
    secDrawUs = secPushUs = secDrawFrames = 0;
}

void clockTick(uint32_t now) {
    uint32_t n, us;
    if (!clkCumValid && now - liveStartMs >= 2000) {  // skip the ES8311 warm-up
        tonemap_audio::chunkClock(n, us);
        clkCumN0 = clkWinN0 = n;
        clkCumUs0 = clkWinUs0 = us;
        clkWinStartMs = now;
        clkCumValid = true;
        return;
    }
    if (!clkCumValid || now - clkWinStartMs < 10000) return;
    tonemap_audio::chunkClock(n, us);
    const double nominal = (double)tonemap_audio::kSampleRate;
    const double hop = (double)tonemap_audio::kHop;
    const double winS = (double)(uint32_t)(us - clkWinUs0) / 1e6;
    const double cumS = (double)(uint32_t)(us - clkCumUs0) / 1e6;
    const double winRate = winS > 0 ? (double)(n - clkWinN0) * hop / winS : 0.0;
    const double cumRate = cumS > 0 ? (double)(n - clkCumN0) * hop / cumS : 0.0;
    Serial.printf(
        "[clock] window=%.2fs chunks=%u chunks/s=%.4f fs_est=%.1f ratio=%.5f | cumulative=%.1fs chunks=%u fs_est=%.1f "
        "ratio=%.5f | nominal chunks/s=31.2500 trim_in_use=%.5f\n",
        winS, (unsigned)(n - clkWinN0), winS > 0 ? (double)(n - clkWinN0) / winS : 0.0, winRate, winRate / nominal, cumS,
        (unsigned)(n - clkCumN0), cumRate, cumRate / nominal, (double)tonemap_audio::kRateTrim);
    clkWinN0 = n;
    clkWinUs0 = us;
    clkWinStartMs = now;
}

void livePrint(uint32_t now) {
    if (now - lastLinePrintMs < 100) return;
    lastLinePrintMs = now;
    const tonemap_audio::Stats st = tonemap_audio::stats();
    Serial.printf(
        "[live] note=%s cents=%+.1f hz=%.1f clar=%.2f rms=%.0f an/s=%.1f chunks/s=%.1f loop_ms=%.1f starved=%u torn=%u "
        "skipped=%u an_ms=%.1f/%.1f fp/s=%.1f loop_max_ms=%.1f recfail=%u badcb=%u draw=%s draw_ms=%.1f push_ms=%.1f\n",
        last.note, last.cents, last.hz, last.clar, last.rms, pubAnPerS, pubChunksPerS, pubLoopMs, (unsigned)st.starved,
        (unsigned)st.torn, (unsigned)st.skipped, pubAnMsAvg, pubAnMsMax, pubFullPerS, pubLoopMaxMs, (unsigned)st.recFail,
        (unsigned)st.badCb, drawOn ? "on" : "off", pubDrawMs, pubPushMs);
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, false);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.println("ToneMap bench - see serial");

    // USB CDC re-enumerates after a reset: wait for the host (up to 30 s) so the first lines are not lost.
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 30000) delay(10);
    delay(500);

    audio::begin();

    printHeader();
    mt::runModelTests();
    runSynthetic();
    runTiming();

    // Live capture exactly as ToneMap's enter() does it: attach, micStart, start.
    Serial.println("[stage] attach");
    tonemap_audio::attach();
    Serial.println("[stage] micStart");
    liveRunning = audio::micStart();
    Serial.printf("[stage] micStart returned %d\n", (int)liveRunning);
    if (liveRunning) tonemap_audio::start();
    Serial.println("[stage] start done");
    Serial.printf("[live] microphone %s, fs=%u, window=%u hop=%u\n", liveRunning ? "started" : "FAILED to start",
                  (unsigned)tonemap_audio::kSampleRate, (unsigned)tonemap_audio::kWindow, (unsigned)tonemap_audio::kHop);

    const uint32_t now = millis();
    liveStartMs = secStartMs = lastLinePrintMs = lastReportMs = now;
    tonemap_audio::chunkClock(secChunkN0, secChunkUs0);
}

void loop() {
    const uint32_t nowUs = micros();
    if (lastLoopUs != 0) {
        const uint32_t d = nowUs - lastLoopUs;
        ++secLoops;
        if (d > secLoopUsMax) secLoopUsMax = d;
    }
    lastLoopUs = nowUs;
    const uint32_t now = millis();

    handleSerial();
    audio::update();
    if (liveRunning) {
        tonemap_audio::service();
        tonemap_audio::WindowInfo wi;
        if (tonemap_audio::takeWindow(sig, wi)) analyse();
    }

    if (drawOn && now - lastDrawMs >= 50) {
        lastDrawMs = now;
        drawLoadStep();
    }

    secondTick(now);
    if (liveRunning) {
        clockTick(now);
        livePrint(now);
    }
    if (now - lastReportMs >= 10000) {
        lastReportMs = now;
        reprintReport();
    }

    delay(4);
}

#endif  // TONEMAP_BENCH
