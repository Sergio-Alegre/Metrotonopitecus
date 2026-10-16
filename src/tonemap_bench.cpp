// ToneMap bench firmware (env cardputer-adv-tonemap-bench, -DTONEMAP_BENCH=1).
// Replaces the normal app: it measures the detector and the capture path and prints
// everything over serial, in English, one parseable line per fact.
//
//   [bench]  header: build, CPU, heap, buffer sizes
//   [synth]  detector accuracy on generated signals, 16 and 32 kHz, then a SUMMARY per rate
//   [time]   microseconds per analysis (avg / max of 50 runs)
//   [live]   microphone mode, ~10 lines/s
//   [clock]  measured I2S chunk rate every 10 s (real sample rate vs nominal)
//   [load]   draw-load toggle messages
//
// Serial keys (send one character):  d  toggle simulated draw load (240x125 sprite every 50 ms)
//                                    r  re-print header + SUMMARY + timing lines
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
constexpr int kMaxReport = 24;
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
    rep("[bench] sizeof(NoteState)=n/a sizeof(Model)=n/a budget=40960 (map model comes in a later phase)");
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
