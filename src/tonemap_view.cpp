// ToneMap view of the tuner. See tonemap_view.h.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// Schedule inside the existing loop() (docs/TONEMAP_DESIGN.md section 6):
//   update(): capture service -> newest 2048-sample window -> prepare -> noise gate ->
//             findPitch -> model.tick; LED; timers
//   draw():   only when something changed, at most ~20 fps, deferred one iteration when an
//             analysis just ran
//   main:     pushSprite + waitDisplay -> afterPush()
//
// Hardware notes (LED, brightness, microphone) are in docs/SPEC.md and CLAUDE.md. The LED shares
// the backlight pin (GPIO38): it only works with the screen at full brightness, and a command sent
// while the backlight PWM is low is lost, so every LED command is repeated on later iterations.
#include "tonemap_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "settings.h"
#include "tonemap_audio.h"
#include "tonemap_detect.h"
#include "tonemap_model.h"

namespace tonemap {

namespace {

// ---- analysis ----------------------------------------------------------------------------
// The window analysis (noise gate table, clarity and frequency gates, detector) is shared with the
// classic tuner: tonemap_detect.h.
using tonemap_detect::kGateRms;
constexpr uint32_t kNoWindowMs = 300;  // an active note is dropped when no window arrives for this long

// ---- screen (canvas 240 x 125) -----------------------------------------------------------
constexpr int kCols = 12;
constexpr int kRows = 6;
constexpr int kCellW = 20;
constexpr int kCellH = 18;
constexpr int kGridY = 17;       // top of the grid; rows 17..124
constexpr int kSepY = 15;        // 1 px separator under the info strip
constexpr int kTextY = 3;        // Font0 text row of the strip
constexpr int kMeterX = 206;     // level meter, 30 px wide
constexpr int kMeterW = 30;
constexpr int kMeterY = 4;
constexpr int kMeterH = 7;
constexpr uint32_t kMinDrawGapMs = 50;   // 20 fps
constexpr uint32_t kOverdueMs = 80;      // a pending frame is drawn even right after an analysis
constexpr uint32_t kFadePollMs = 500;    // redraw timer while some cell is fading
constexpr uint32_t kMessageMs = 1500;
constexpr int kMenuPitch = 9;            // options overlay: line pitch (13 lines fit the 125 px canvas)
constexpr int kMenuValueCols = 13;       // label column width in characters (value column)
constexpr int kActiveBarH = 2;            // active-cell white bar along the bottom of the cell: height in px

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kColBlack = 0x0000;
constexpr uint16_t kColWhite = 0xFFFF;
// In the grid white belongs to the active cell only (its bar and its label). Every other label is black or this
// light grey, chosen by the luminance of the colour of the cell.
constexpr uint16_t kColLabelLight = rgb565(190, 190, 190);
constexpr uint16_t kColGrid = rgb565(0x37, 0x41, 0x51);      // #374151
constexpr uint16_t kColLabelOff = rgb565(0x4b, 0x55, 0x63);  // #4b5563 (never visited)
constexpr uint16_t kColNeedle = rgb565(0xef, 0x44, 0x44);    // #ef4444
constexpr uint16_t kColGreen = rgb565(34, 197, 94);
constexpr uint16_t kColGrey = rgb565(156, 163, 175);
constexpr uint16_t kColDim = rgb565(107, 114, 128);
constexpr uint16_t kColMeterBg = rgb565(31, 41, 55);
constexpr uint16_t kColInfo = rgb565(125, 211, 252);         // transient messages
constexpr uint16_t kColPaused = rgb565(251, 191, 36);
constexpr uint16_t kColRefA = rgb565(255, 255, 0);
constexpr uint16_t kColTransp = rgb565(103, 232, 249);
constexpr uint16_t kColError = rgb565(239, 68, 68);

// ---- LED (copy of the minimum from mode_metronome.cpp) -----------------------------------
constexpr int kLedPin = 21;
constexpr uint8_t kLedMinBrightness = 255;
constexpr int kLedResends = 2;
constexpr uint32_t kLedResendMs = 6;
constexpr int kLedOffSends = 3;
constexpr int kLedStep = 16;             // a new live colour is sent when a channel moved by this much ...
constexpr uint32_t kLedMinGapMs = 50;    // ... and at most this often (note start / stop are never delayed)

const char* const kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
const char* const kSensNames[3] = {"Relaxed", "Medium", "Hard"};
const char* const kHistNames[4] = {"1 s", "3 s", "30 s", "Unlimited"};
const char* const kHoldNames[6] = {"5 s", "15 s", "30 s", "2 min", "5 min", "No fade"};
const char* const kGateNames[4] = {"None", "Low", "Med", "High"};
const char* const kTranspNames[4] = {"C", "Bb", "Eb", "F"};
const char* const kPaletteNames[2] = {"Safe", "Extreme"};

// ---- state -------------------------------------------------------------------------------
// The heat map: ~38 KB in .bss (Model has an empty constructor), never on the stack. It lives as
// long as the firmware runs, so it survives mode and view switches.
Model model;

// Map clock: millis() minus the time spent paused, so the fades freeze while paused.
uint32_t clockOffset = 0;
bool paused = false;
uint32_t pauseStart = 0;

bool active = false;       // between enter() and leave()
bool menuOpen = false;     // options overlay (Alt / Enter); always closed on enter() and leave()
bool micOk = false;
bool dirty = true;
bool analysedThisLoop = false;
uint32_t lastDrawMs = 0;
uint32_t lastWindowMs = 0;
uint32_t lastFadePollMs = 0;

// Active note (as shown).
bool hasNote = false;
int writtenMidi = 0;
float dispCents = 0.0f;
float noteHz = 0.0f;
bool noteInZone = false;   // raw |errRatio| inside the dead zone of the sensitivity mode
float liveErr = 0.0f;      // raw errRatio of the current frame (live colour of the cents text and of the LED)
float level = 0.0f;        // smoothed window RMS (int16 units)
bool gateOpen = false;     // the last window passed the noise gate

// Transient message.
char msgText[32];
bool msgActive = false;
uint32_t msgUntil = 0;

// LED + brightness.
bool brightRaised = false;
uint8_t userBrightness = 128;
bool ledLit = false;       // the LED shows the live colour of the active note
uint32_t ledSentMs = 0;    // when the live colour was last sent
int ledResends = 0;
uint32_t ledResendAt = 0;
uint8_t ledR = 0, ledG = 0, ledB = 0;

// What the last drawn frame showed, to decide whether a new frame is needed.
struct Snapshot {
    bool hasNote;
    bool inZone;
    bool gateOpen;
    int16_t midi;
    int16_t centsTenths;
    int32_t hzTenths;
    int8_t needleRow;
    uint8_t meterPx;
    uint16_t cellColor;
    uint16_t liveCol;
};
Snapshot drawn = {};

#ifdef TONEMAP_DEBUG
struct Dbg {
    uint32_t periodStartMs;
    uint32_t windows, anUsSum, anUsMax;
    uint32_t draws, drawUsSum;
    uint32_t pushes, pushUsSum;
    uint32_t loops, loopUsSum, lastLoopUs;
    uint32_t drawEndUs;
    bool pushPending;
    float rms, clar, hz;
    float cents;
    char note[8];
    // published once per period (overlay + serial)
    float pubAnPerS, pubAnMsAvg, pubAnMsMax, pubDrawMs, pubPushMs, pubFps, pubLoopMs;
};
Dbg dbg;
#endif

// ---- helpers -----------------------------------------------------------------------------
inline uint32_t mapNow() { return (paused ? pauseStart : (uint32_t)millis()) - clockOffset; }

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int gateIdx() { return clampi(settings::get().tmGate, 0, 3); }

// Written note name, e.g. "A#4" / "Bb4". Caller checks the range.
void noteName(int midi, bool flats, char* out, size_t n) {
    const int pc = ((midi % 12) + 12) % 12;
    snprintf(out, n, "%s%d", flats ? kFlatNames[pc] : kSharpNames[pc], midi / 12 - 1);
}

// Level meter: -60 dBFS .. -6 dBFS -> 0..1 (docs/TONEMAP_DESIGN.md section 6).
float levelFrac(float rms) {
    if (!(rms >= 1.0f)) return 0.0f;
    const float db = 20.0f * log10f(rms / 32768.0f);
    return clampf((db + 60.0f) / 54.0f, 0.0f, 1.0f);
}

int meterPixels(float rms) { return (int)(levelFrac(rms) * (float)kMeterW + 0.5f); }

// First of the two rows (0..16) of the red needle inside a cell of kCellH rows. Its centre sits at
// 90 % - pct * 80 % of the cell height (pct 0.5 is the black reference line).
int needleRow(float cents) {
    const float pct = needlePct(cents);
    const float centre = (float)kCellH * (0.9f - 0.8f * pct);
    return clampi((int)lroundf(centre) - 1, 0, kCellH - 2);
}

bool inGrid(int midi) { return midi >= kGridLo && midi <= kGridHi; }

void showMessage(const char* fmt, const char* arg) {
    snprintf(msgText, sizeof(msgText), fmt, arg);
    msgActive = true;
    msgUntil = millis() + kMessageMs;
    dirty = true;
}

// ---- LED and brightness ------------------------------------------------------------------
// The LED (SK6812) is on GPIO21 and M5.Led does nothing with this Arduino core.
void ledSend(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(kLedPin, r, g, b); }

// Sets the LED now and schedules the repeats that make up for a command lost to the backlight PWM.
void ledShow(uint8_t r, uint8_t g, uint8_t b) {
    ledR = r;
    ledG = g;
    ledB = b;
    ledSend(r, g, b);
    ledResends = kLedResends;
    ledResendAt = millis() + kLedResendMs;
}

// Blocking off: used before the brightness is restored, so that the commands are sent while the
// backlight pin is still held high. Only does something if the LED may be on.
void ledOffBlocking() {
    if (ledLit || ledResends > 0) {
        for (int i = 0; i < kLedOffSends; ++i) {
            ledSend(0, 0, 0);
            delay(kLedResendMs);
        }
    }
    ledLit = false;
    ledResends = 0;
    ledR = ledG = ledB = 0;
}

void brightnessRaise() {
    if (brightRaised) return;
    const uint8_t b = M5Cardputer.Display.getBrightness();
    if (b < kLedMinBrightness) {
        userBrightness = b;
        M5Cardputer.Display.setBrightness(kLedMinBrightness);
        brightRaised = true;
    }
}

void brightnessRestore() {
    if (!brightRaised) return;
    M5Cardputer.Display.setBrightness(userBrightness);
    brightRaised = false;
}

// Live colour of the note sounding now: the instantaneous deviation (raw per-frame errRatio) in the active palette,
// full intensity (docs/TONEMAP_DESIGN.md section 13.8).
Rgb liveRgb() { return liveColor(model.palette(), model.sensitivity(), liveErr); }

inline int absDiff(int a, int b) { return a > b ? a - b : b - a; }

// Called every loop. LED on + active note + not paused: the LED shows the live colour, sent when the note starts and
// then only when a channel moved by kLedStep or more (and at most every kLedMinGapMs). Otherwise off.
void ledUpdate() {
    const bool want = settings::get().tmLed != 0 && !paused && hasNote;
    const uint32_t nowMs = millis();
    if (!want) {
        if (ledLit) {
            ledLit = false;
            ledShow(0, 0, 0);
        }
    } else {
        const Rgb c = liveRgb();
        const bool moved = absDiff(c.r, ledR) >= kLedStep || absDiff(c.g, ledG) >= kLedStep || absDiff(c.b, ledB) >= kLedStep;
        if (!ledLit || (moved && (uint32_t)(nowMs - ledSentMs) >= kLedMinGapMs)) {
            ledLit = true;
            ledSentMs = nowMs;
            ledShow(c.r, c.g, c.b);
        }
    }
    if (ledResends > 0 && (int32_t)(millis() - ledResendAt) >= 0) {
        ledSend(ledR, ledG, ledB);
        --ledResends;
        ledResendAt += kLedResendMs;
    }
}

// ---- model <-> settings ------------------------------------------------------------------
// Silent sync: a setter is only called when its value differs (they restart the fade timers).
void syncModel(uint32_t now) {
    const auto& s = settings::get();
    if ((int)model.sensitivity() != s.tmSens) model.setSensitivity((Sensitivity)clampi(s.tmSens, 0, 2), now);
    if (model.historyIdx() != s.tmHist) model.setHistory(s.tmHist, now);
    if (model.holdIdx() != s.tmHold) model.setHold(s.tmHold, now);
    if (model.palette() != s.tmPalette) model.setPalette(s.tmPalette);  // colours only: no timers
    if (fabsf(model.refA() - s.refA) > 1e-4f) model.setRefA(s.refA, now);
    if (model.transposition() != s.tmTransp) model.setTransposition(s.tmTransp, now);
}

int transpIndex(int semis) {
    for (int i = 0; i < 4; ++i) {
        if (kTransposeSemis[i] == semis) return i;
    }
    return 0;
}

Snapshot makeSnapshot() {
    Snapshot s = {};
    s.hasNote = hasNote;
    s.inZone = hasNote && noteInZone;
    if (hasNote) {
        const Rgb lc = liveRgb();
        s.liveCol = rgb565(lc.r, lc.g, lc.b);
    }
    s.gateOpen = gateOpen;
    s.meterPx = (uint8_t)meterPixels(level);
    if (hasNote) {
        s.midi = (int16_t)writtenMidi;
        s.centsTenths = (int16_t)lroundf(dispCents * 10.0f);
        s.hzTenths = (int32_t)lroundf(noteHz * 10.0f);
        s.needleRow = (int8_t)needleRow(dispCents);
        if (inGrid(writtenMidi)) {
            const Rgb c = model.cellColor(writtenMidi, mapNow(), nullptr);
            s.cellColor = rgb565(c.r, c.g, c.b);
        }
    }
    return s;
}

bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    return a.hasNote == b.hasNote && a.inZone == b.inZone && a.gateOpen == b.gateOpen && a.midi == b.midi &&
           a.centsTenths == b.centsTenths && a.hzTenths == b.hzTenths && a.needleRow == b.needleRow &&
           a.meterPx == b.meterPx && a.cellColor == b.cellColor && a.liveCol == b.liveCol;
}

// ---- analysis ----------------------------------------------------------------------------
void analyse(const tonemap_audio::WindowInfo& wi) {
#ifdef TONEMAP_DEBUG
    const uint32_t t0 = micros();
#endif
    const tonemap_detect::Analysis an = tonemap_detect::analyse(kGateRms[gateIdx()]);
    const bool loud = an.loud;
    const bool valid = an.valid;
    const float hz = an.hz;

    const TickResult t = model.tick(valid, hz, wi.dtMs, mapNow());

    hasNote = t.hasNote;
    if (t.hasNote) {
        writtenMidi = t.writtenMidi;
        dispCents = t.displayCents;
        noteHz = t.hz;
        noteInZone = t.inDeadzone;
        liveErr = t.errRatio;
    } else {
        noteInZone = false;
    }
    gateOpen = loud;

    // Level meter: instant attack, ~200 ms release.
    level = an.rms > level ? an.rms : level * 0.85f + an.rms * 0.15f;
    lastWindowMs = millis();
    analysedThisLoop = true;

    // While the options overlay is open only key presses redraw (it shows no analysis data).
    if (!menuOpen && !sameSnapshot(makeSnapshot(), drawn)) dirty = true;

#ifdef TONEMAP_DEBUG
    const uint32_t us = micros() - t0;
    ++dbg.windows;
    dbg.anUsSum += us;
    if (us > dbg.anUsMax) dbg.anUsMax = us;
    dbg.rms = an.rms;
    dbg.clar = an.clarity;
    dbg.hz = hz;
    dbg.cents = t.hasNote ? t.displayCents : 0.0f;
    if (t.hasNote && inGrid(t.writtenMidi)) {
        noteName(t.writtenMidi, settings::get().tmFlats != 0, dbg.note, sizeof(dbg.note));
    } else {
        strlcpy(dbg.note, "--", sizeof(dbg.note));
    }
#endif
}

void dropNote() {
    model.endNote();
    hasNote = false;
    noteInZone = false;
}

// ---- pause -------------------------------------------------------------------------------
void setPaused(bool p) {
    if (p == paused || !micOk) return;
    if (p) {
        tonemap_audio::stop();
        dropNote();
        level = 0.0f;
        gateOpen = false;
        pauseStart = millis();
        paused = true;  // from here on mapNow() is frozen
    } else {
        clockOffset += (uint32_t)millis() - pauseStart;
        paused = false;
        lastWindowMs = millis();
        tonemap_audio::start();  // deferred to service() while the previous chunks drain
    }
    Serial.printf("[tonemap] %s\n", p ? "paused" : "resumed");
    dirty = true;
}

#ifdef TONEMAP_DEBUG
void debugPeriod(uint32_t now) {
    if (now - dbg.periodStartMs < 500) return;
    const float el = (float)(now - dbg.periodStartMs) / 1000.0f;
    dbg.pubAnPerS = (float)dbg.windows / el;
    dbg.pubAnMsAvg = dbg.windows ? (float)dbg.anUsSum / (float)dbg.windows / 1000.0f : 0.0f;
    dbg.pubAnMsMax = (float)dbg.anUsMax / 1000.0f;
    dbg.pubDrawMs = dbg.draws ? (float)dbg.drawUsSum / (float)dbg.draws / 1000.0f : 0.0f;
    dbg.pubPushMs = dbg.pushes ? (float)dbg.pushUsSum / (float)dbg.pushes / 1000.0f : 0.0f;
    dbg.pubFps = (float)dbg.draws / el;
    dbg.pubLoopMs = dbg.loops ? (float)dbg.loopUsSum / (float)dbg.loops / 1000.0f : 0.0f;
    dbg.periodStartMs = now;
    dbg.windows = dbg.anUsSum = dbg.anUsMax = 0;
    dbg.draws = dbg.drawUsSum = dbg.pushes = dbg.pushUsSum = 0;
    dbg.loops = dbg.loopUsSum = 0;

    const tonemap_audio::Stats st = tonemap_audio::stats();
    Serial.printf(
        "[tm] an/s=%.1f an_ms=%.1f/%.1f draw_ms=%.1f push_ms=%.1f fps=%.1f loop_ms=%.1f rms=%.0f clar=%.2f hz=%.1f "
        "note=%s %+.1fc gate=%s starved=%u torn=%u skipped=%u heap=%u minheap=%u\n",
        (double)dbg.pubAnPerS, (double)dbg.pubAnMsAvg, (double)dbg.pubAnMsMax, (double)dbg.pubDrawMs,
        (double)dbg.pubPushMs, (double)dbg.pubFps, (double)dbg.pubLoopMs, (double)dbg.rms, (double)dbg.clar,
        (double)dbg.hz, dbg.note, (double)dbg.cents, kGateNames[gateIdx()], (unsigned)st.starved, (unsigned)st.torn,
        (unsigned)st.skipped, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    if (!menuOpen) dirty = true;  // refresh the overlay
}

void debugOverlay(M5Canvas& c) {
    // 40 characters fit the 240 px width (Font0 is 6 px per character).
    char buf[56];
    snprintf(buf, sizeof(buf), "a/s%.1f an%.1f dr%.1f pu%.1f rms%.0f cl%.2f", (double)dbg.pubAnPerS,
             (double)dbg.pubAnMsAvg, (double)dbg.pubDrawMs, (double)dbg.pubPushMs, (double)dbg.rms,
             (double)dbg.clar);
    c.fillRect(0, 117, 240, 8, kColBlack);
    c.setFont(&fonts::Font0);
    c.setTextSize(1);
    c.setTextDatum(top_left);
    c.setTextColor(rgb565(255, 165, 0));
    c.drawString(buf, 0, 117);
}
#endif

// ---- drawing -----------------------------------------------------------------------------
void drawCell(M5Canvas& c, int row, int col, uint32_t now, bool flats) {
    const int midi = 12 * (8 - row) + col;  // WRITTEN midi: row 0 is octave 7, row 5 octave 2
    const int x = col * kCellW;
    const int y = kGridY + row * kCellH;

    bool visited = false;
    const Rgb rgb = model.cellColor(midi, now, &visited);
    if (rgb.r | rgb.g | rgb.b) c.fillRect(x, y, kCellW, kCellH, rgb565(rgb.r, rgb.g, rgb.b));

    c.drawFastVLine(x + kCellW - 1, y, kCellH, kColGrid);
    c.drawFastHLine(x, y + kCellH - 1, kCellW, kColGrid);

    char label[8];
    if (col == 0) {
        snprintf(label, sizeof(label), "C%d", midi / 12 - 1);
    } else {
        strlcpy(label, flats ? kFlatNames[col] : kSharpNames[col], sizeof(label));
    }
    // Active cell (the note sounding now): white. Other visited cells: black or light grey by the luminance of
    // the colour shown (the near-white centre of the Safe palette needs black text). Never visited: dim grey on black.
    const bool active = hasNote && writtenMidi == midi;
    c.setTextColor(active ? kColWhite : (!visited ? kColLabelOff : (useBlackLabel(rgb) ? kColBlack : kColLabelLight)));
    c.drawString(label, x + 2, y + 2);
}

// The active cell (the only place with white in the grid, besides its label): a white bar along the bottom of
// the cell, kCellW - 1 wide (inside the grid line on the right) and kActiveBarH high, resting on the grid line at
// the bottom edge; then the black reference line at the vertical centre and, last, the red needle, which stays
// visible where it crosses the bar. No outline, no shadow.
void drawActiveCell(M5Canvas& c, uint32_t /*now*/) {
    if (!hasNote || !inGrid(writtenMidi)) return;
    const int row = 7 - (writtenMidi / 12 - 1);  // octave 7 is the top row
    const int col = writtenMidi % 12;
    const int x = col * kCellW;
    const int y = kGridY + row * kCellH;
    c.fillRect(x, y + kCellH - 1 - kActiveBarH, kCellW - 1, kActiveBarH, kColWhite);
    c.fillRect(x + 2, y + kCellH / 2 - 1, kCellW - 4, 2, kColBlack);
    c.fillRect(x, y + needleRow(dispCents), kCellW, 2, kColNeedle);
}

void drawStrip(M5Canvas& c) {
    const auto& s = settings::get();
    const bool flats = s.tmFlats != 0;

    c.setFont(&fonts::Font0);
    c.setTextSize(1);
    c.setTextDatum(top_left);

    // Left: transient message > pause > microphone error > note, cents, Hz. leftEnd = right edge in px.
    int leftEnd = 2;
    if (msgActive) {
        c.setTextColor(kColInfo);
        c.drawString(msgText, 2, kTextY);
        leftEnd = 2 + c.textWidth(msgText);
    } else if (paused) {
        c.setTextColor(kColPaused);
        c.drawString("Paused (Space)", 2, kTextY);
        leftEnd = 2 + c.textWidth("Paused (Space)");
    } else if (!micOk) {
        c.setTextColor(kColError);
        c.drawString("Microphone unavailable", 2, kTextY);
        leftEnd = 2 + c.textWidth("Microphone unavailable");
    } else if (!hasNote) {
        c.setTextColor(kColDim);
        c.drawString("--", 2, kTextY);
        leftEnd = 2 + c.textWidth("--");
    } else if (writtenMidi < kGridLo) {
        c.setTextColor(kColGrey);
        c.drawString("Below range", 2, kTextY);
        leftEnd = 2 + c.textWidth("Below range");
    } else if (writtenMidi > kGridHi) {
        c.setTextColor(kColGrey);
        c.drawString("Above range", 2, kTextY);
        leftEnd = 2 + c.textWidth("Above range");
    } else {
        char buf[16];
        noteName(writtenMidi, flats, buf, sizeof(buf));
        c.setTextColor(kColWhite);
        c.drawString(buf, 2, kTextY);

        const int tenths = (int)lroundf(dispCents * 10.0f);
        const int mag = tenths < 0 ? -tenths : tenths;
        snprintf(buf, sizeof(buf), "%c%d.%dc", tenths < 0 ? '-' : '+', mag / 10, mag % 10);
        Rgb lc = liveRgb();  // live colour of the instantaneous deviation, active palette
        // Text only (the LED keeps the true colour): very dark colours, i.e. pure blue, are lightened toward
        // white so the small Font0 digits stay readable on the black strip (pure blue is ~2.4:1 contrast).
        {
            const float lum = (0.2126f * lc.r + 0.7152f * lc.g + 0.0722f * lc.b) / 255.0f;
            float k = (0.18f - lum) / 0.18f;
            k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
            k *= 0.5f;
            lc.r = (uint8_t)(lc.r + (255 - lc.r) * k + 0.5f);
            lc.g = (uint8_t)(lc.g + (255 - lc.g) * k + 0.5f);
            lc.b = (uint8_t)(lc.b + (255 - lc.b) * k + 0.5f);
        }
        c.setTextColor(rgb565(lc.r, lc.g, lc.b));
        c.drawString(buf, 26, kTextY);

        snprintf(buf, sizeof(buf), "%.1f Hz", (double)noteHz);
        c.setTextColor(kColGrey);
        c.drawString(buf, 70, kTextY);
        leftEnd = 70 + c.textWidth(buf);
    }

    // Right: transposition (if not C), A reference, level meter with the gate mark.
    c.setTextDatum(top_right);
    char ref[12];
    snprintf(ref, sizeof(ref), "A=%.0f", (double)s.refA);
    const int refRight = kMeterX - 6;
    c.setTextColor(kColRefA);
    c.drawString(ref, refRight, kTextY);
    int rightStart = refRight - c.textWidth(ref);  // left edge of the right-hand block
    if (s.tmTransp != 0) {
        const char* tn = kTranspNames[transpIndex(s.tmTransp)];
        c.setTextColor(kColTransp);
        c.drawString(tn, refRight - (int)strlen(ref) * 6 - 6, kTextY);
        rightStart -= 6 + c.textWidth(tn);
    }
    c.setTextDatum(top_left);

    // Hint for the options overlay, just left of the right-hand block, only where it does not collide with
    // the note / cents / Hz block (4 px margin on both sides). Full "Alt:menu" (48 px), else "Alt" (18 px).
    {
        const int hintRight = rightStart - 4;
        const char* hint = nullptr;
        if (hintRight - c.textWidth("Alt:menu") >= leftEnd + 4) {
            hint = "Alt:menu";
        } else if (hintRight - c.textWidth("Alt") >= leftEnd + 4) {
            hint = "Alt";
        }
        if (hint) {
            c.setTextColor(kColDim);
            c.drawString(hint, hintRight - c.textWidth(hint), kTextY);
        }
    }

    c.fillRect(kMeterX, kMeterY, kMeterW, kMeterH, kColMeterBg);
    const int px = (micOk && !paused) ? meterPixels(level) : 0;
    if (px > 0) c.fillRect(kMeterX, kMeterY, px, kMeterH, gateOpen ? kColGreen : kColDim);
    const float gate = kGateRms[gateIdx()];
    if (gate > 0.0f) {
        const int gx = kMeterX + clampi(meterPixels(gate), 0, kMeterW - 1);
        c.drawFastVLine(gx, kMeterY - 1, kMeterH + 2, kColWhite);
    }

    c.drawFastHLine(0, kSepY, 240, kColGrid);
}

// ---- options overlay (Alt / Enter) ---------------------------------------------------------------------
// One line of the overlay: key (yellow), label (grey), optional current value (white) in a fixed column.
void drawMenuRow(M5Canvas& c, int row, const char* key, const char* label, const char* value) {
    const int y = 2 + row * kMenuPitch;
    int x = 4;
    c.setTextColor(kColRefA);
    c.drawString(key, x, y);
    x += (int)strlen(key) * 6 + 6;
    c.setTextColor(kColGrey);
    c.drawString(label, x, y);
    if (value) {
        c.setTextColor(kColWhite);
        c.drawString(value, x + kMenuValueCols * 6, y);
    }
}

// Full-canvas overlay with the keys and their CURRENT values (13 lines of Font0, <= 40 characters each).
void drawMenu(M5Canvas& c) {
    const auto& s = settings::get();
    c.fillSprite(kColBlack);
    c.setFont(&fonts::Font0);
    c.setTextSize(1);
    c.setTextDatum(top_left);

    c.setTextColor(kColInfo);
    c.drawString("OPTIONS", 4, 2);
    c.setTextDatum(top_right);
    c.setTextColor(kColDim);
    c.drawString("Alt/Enter: close", 236, 2);
    c.setTextDatum(top_left);

    drawMenuRow(c, 1, "S", "Sensitivity", kSensNames[clampi(s.tmSens, 0, 2)]);
    drawMenuRow(c, 2, "H", "History", kHistNames[clampi(s.tmHist, 0, 3)]);
    drawMenuRow(c, 3, "C", "Color hold", kHoldNames[clampi(s.tmHold, 0, 5)]);
    drawMenuRow(c, 4, "N", "Noise gate", kGateNames[clampi(s.tmGate, 0, 3)]);
    drawMenuRow(c, 5, "K", "Transpose", kTranspNames[transpIndex(s.tmTransp)]);
    drawMenuRow(c, 6, "F", "Accidentals", s.tmFlats ? "flats" : "sharps");
    drawMenuRow(c, 7, "P", "Palette", kPaletteNames[clampi(s.tmPalette, 0, 1)]);
    drawMenuRow(c, 8, "L", "LED", s.tmLed ? "on" : "off");
    drawMenuRow(c, 9, "R", "Clear map", nullptr);
    drawMenuRow(c, 10, "Space", paused ? "Resume" : "Pause", nullptr);
    char ref[32];
    snprintf(ref, sizeof(ref), " A=%.0f (reference)", (double)s.refA);
    drawMenuRow(c, 11, "Up/Down", ref, nullptr);
    drawMenuRow(c, 12, "T", "Classic view", nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------------------
void enter() {
    if (active) leave();
    active = true;
    menuOpen = false;
#ifdef TONEMAP_DEBUG
    Serial.begin(115200);  // the app never opens the USB CDC port by itself: without this no [tm] line reaches the PC
#endif
    Serial.println("[tonemap] enter");

    // The mic is ended on every entry path; be safe if it is not (attach() needs it stopped).
    if (audio::micOn()) audio::micStop();
    tonemap_audio::attach();
    micOk = audio::micStart();
    if (!micOk) Serial.println("[tonemap] microphone unavailable");

    if (settings::get().tmLed != 0) brightnessRaise();
    paused = false;
    syncModel(mapNow());

    hasNote = false;
    noteInZone = false;
    gateOpen = false;
    level = 0.0f;
    msgActive = false;
    ledLit = false;
    ledResends = 0;
    analysedThisLoop = false;
    lastWindowMs = millis();
    lastFadePollMs = millis();
    lastDrawMs = millis() - 1000;
    dirty = true;
    drawn = Snapshot{};
#ifdef TONEMAP_DEBUG
    dbg = Dbg{};
    strlcpy(dbg.note, "--", sizeof(dbg.note));
    dbg.periodStartMs = millis();
    dbg.lastLoopUs = micros();
#endif

    if (micOk && !tonemap_audio::start()) micOk = false;
}

void leave() {
    if (!active) return;
    active = false;
    menuOpen = false;
    Serial.println("[tonemap] leave");

    tonemap_audio::stop();
    ledOffBlocking();          // while the backlight pin is still held at full brightness
    brightnessRestore();
    audio::micStop();          // waits for the chunks in flight, then brings the speaker back
    tonemap_audio::detach();   // back to the 16 kHz default (the other modes never record)
    dropNote();
    if (paused) {              // settle the pause: the map clock stays frozen across the pause and the time away
        clockOffset += (uint32_t)millis() - pauseStart;
        paused = false;
    }
    msgActive = false;
}

void onKey(const KeyEvent& e) {
    if (!active) return;
    auto& s = settings::get();
    const uint32_t now = mapNow();

    switch (e.key) {
        case Key::Up:
        case Key::Down: {
            const float next = constrain(s.refA + (e.key == Key::Up ? 1.0f : -1.0f), settings::kMinRefA,
                                         settings::kMaxRefA);
            if (next != s.refA) {
                s.refA = next;
                model.setRefA(next, now);
                settings::touch();
            }
            break;
        }
        case Key::Space:
            setPaused(!paused);
            break;
        case Key::Alt:
        case Key::Enter:
            menuOpen = !menuOpen;  // options overlay; the dirty flag below forces a full redraw either way
            break;
        case Key::Char:
            switch (e.ch) {
                case 's':
                    s.tmSens = (clampi(s.tmSens, 0, 2) + 1) % 3;
                    model.setSensitivity((Sensitivity)s.tmSens, now);
                    settings::touch();
                    showMessage("Sensitivity: %s", kSensNames[s.tmSens]);
                    break;
                case 'h':
                    s.tmHist = (clampi(s.tmHist, 0, 3) + 1) % 4;
                    model.setHistory(s.tmHist, now);
                    settings::touch();
                    showMessage("History: %s", kHistNames[s.tmHist]);
                    break;
                case 'c':
                    s.tmHold = (clampi(s.tmHold, 0, 5) + 1) % 6;
                    model.setHold(s.tmHold, now);
                    settings::touch();
                    showMessage("Color hold: %s", kHoldNames[s.tmHold]);
                    break;
                case 'n':
                    s.tmGate = (clampi(s.tmGate, 0, 3) + 1) % 4;
                    settings::touch();
                    showMessage("Noise gate: %s", kGateNames[s.tmGate]);
                    break;
                case 'k': {
                    const int idx = (transpIndex(s.tmTransp) + 1) % 4;
                    s.tmTransp = kTransposeSemis[idx];
                    model.setTransposition(s.tmTransp, now);  // also ends the active note
                    hasNote = false;
                    noteInZone = false;
                    settings::touch();
                    showMessage("Transpose: %s", kTranspNames[idx]);
                    break;
                }
                case 'f':
                    s.tmFlats = s.tmFlats ? 0 : 1;
                    settings::touch();
                    showMessage("Accidentals: %s", s.tmFlats ? "flats" : "sharps");
                    break;
                case 'l':
                    s.tmLed = s.tmLed ? 0 : 1;
                    settings::touch();
                    if (s.tmLed) {
                        brightnessRaise();
                    } else {
                        ledOffBlocking();
                        brightnessRestore();
                    }
                    showMessage("LED: %s", s.tmLed ? "on" : "off");
                    break;
                case 'p':
                    s.tmPalette = (clampi(s.tmPalette, 0, 1) + 1) % 2;
                    model.setPalette(s.tmPalette);  // redraw only: no timer restart, no data change
                    settings::touch();
                    showMessage("Palette: %s", kPaletteNames[s.tmPalette]);
                    break;
                case 'r':
                    model.reset(now);
                    hasNote = false;
                    noteInZone = false;
                    showMessage("%s", "Map cleared");
                    break;
                default:
                    break;
            }
            break;
        default:
            break;  // Left / Right / Del: unused in v1
    }
    dirty = true;
}

void update() {
    if (!active) return;
    const uint32_t now = millis();
    analysedThisLoop = false;

#ifdef TONEMAP_DEBUG
    {
        const uint32_t us = micros();
        if (dbg.lastLoopUs) {
            dbg.loopUsSum += us - dbg.lastLoopUs;
            ++dbg.loops;
        }
        dbg.lastLoopUs = us;
    }
#endif

    if (msgActive && (int32_t)(now - msgUntil) >= 0) {
        msgActive = false;
        dirty = true;
    }

    if (micOk) {
        tonemap_audio::service();
        if (!paused) {
            tonemap_audio::WindowInfo wi;
            if (tonemap_audio::takeWindow(tonemap_detect::window(), wi)) {
                analyse(wi);
            } else if (hasNote && (uint32_t)(millis() - lastWindowMs) > kNoWindowMs) {
                // No window for a while (capture restarting): do not keep a stale note on screen.
                dropNote();
                level = 0.0f;
                gateOpen = false;
                dirty = true;
            }
        }
    }

    ledUpdate();

    // Fades change colours without any new audio: poll the model while some cell is fading.
    if (!paused && (uint32_t)(now - lastFadePollMs) >= kFadePollMs) {
        lastFadePollMs = now;
        if (!menuOpen && model.anyFading(mapNow())) dirty = true;
    }

#ifdef TONEMAP_DEBUG
    debugPeriod(now);
#endif
}

bool draw(M5Canvas& c, bool force) {
    if (!active) return false;
    const uint32_t now = millis();
    if (!force) {
        if (!dirty) return false;
        const uint32_t since = now - lastDrawMs;
        if (since < kMinDrawGapMs) return false;
        if (analysedThisLoop && since < kOverdueMs) return false;  // leave this iteration to the analysis
    }
#ifdef TONEMAP_DEBUG
    const uint32_t t0 = micros();
#endif
    dirty = false;
    lastDrawMs = now;

    const uint32_t mnow = mapNow();
    const bool flats = settings::get().tmFlats != 0;

    if (menuOpen) {
        drawMenu(c);
    } else {
        c.fillSprite(kColBlack);
        c.setFont(&fonts::Font0);
        c.setTextSize(1);
        c.setTextDatum(top_left);
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < kCols; ++col) drawCell(c, row, col, mnow, flats);
        }
        drawActiveCell(c, mnow);
        drawStrip(c);
#ifdef TONEMAP_DEBUG
        debugOverlay(c);
#endif
    }

    drawn = makeSnapshot();

#ifdef TONEMAP_DEBUG
    dbg.drawEndUs = micros();
    dbg.drawUsSum += dbg.drawEndUs - t0;
    ++dbg.draws;
    dbg.pushPending = true;
#endif
    return true;
}

void afterPush() {
#ifdef TONEMAP_DEBUG
    if (dbg.pushPending) {
        dbg.pushPending = false;
        dbg.pushUsSum += micros() - dbg.drawEndUs;
        ++dbg.pushes;
    }
#endif
}

}  // namespace tonemap
