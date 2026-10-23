// Tuner (key T).
//   Left / Right  drone note, one semitone per step (C3..B5); detection does not depend on it
//   Up / Down     reference A4 in Hz (default 442, persisted)
//   Space or P    sine drone on the selected note (pauses detection)
//
// Classic view: the tuner listens to the whole range (C2..B7), finds the note nearest to what it
// hears (equal temperament, reference A = refA) and the needle shows how many cents sharp or flat
// you are relative to THAT note. The needle works like an analog meter on an arc of about 100
// degrees: centre = in tune, green band = +-5 cents, full scale = +-50 cents. The detected note is
// written big in the middle of the dial, the cents and the frequency at the top.
//
// The engine is the ToneMap one (tonemap_audio capture at 32 kHz, tonemap_detect window analysis,
// tonemap::freqToNote). While the drone plays the tuner is not listening, and the whole gauge
// turns grey to show it: the ADV microphone and speaker share clock pins, so the microphone is off
// while the drone plays (and the drone would be what the mic hears anyway).
//
// LED (key L, shared on/off setting tmLed with the ToneMap view): the RGB LED lights green while the
// displayed (smoothed) reading is inside the green band (+-5 cents) and is off otherwise: no reading,
// drone playing, mic failed, LED off, view or mode switch. The LED needs the screen at full brightness
// (see tuner_led.h), so the brightness is raised while the LED setting is on and restored on every exit.
#include <math.h>

#include "audio.h"
#include "modes.h"
#include "settings.h"
#include "tonemap_audio.h"
#include "tonemap_detect.h"
#include "tonemap_model.h"
#include "tonemap_view.h"
#include "tuner_led.h"

namespace {

// Validity gate: an AUTOMATIC noise gate (tonemap_detect::AutoGate: 2.5 x the noise floor of the room, clamped to
// 20..80; the ToneMap view keeps its manual table, setting tmGate); clarity and frequency gates are the ones of
// tonemap_detect.
constexpr uint32_t kHoldMs = 700;         // keep the last reading this long without new ones
constexpr float kSmoothTauMs = 185.0f;    // reading smoothing (the old detector did 0.5 per 128 ms frame)
constexpr float kJumpCents = 25.0f;       // larger changes are taken at once
constexpr float kFullScale = 50.0f;       // cents at the ends of the arc
constexpr float kGreenCents = 5.0f;
constexpr float kRestDeg = -52.0f;        // needle parked just past the left end
constexpr uint32_t kFooterMsgMs = 1500;   // "LED: on / off" replaces the footer hint this long

// Gauge geometry: pivot below the visible area so only a slice of the arc shows.
constexpr int kPivotX = 120;
constexpr int kPivotY = 175;
constexpr int kBandInner = 112;
constexpr int kBandOuter = 130;
constexpr int kNeedleInner = 94;          // the inside of the dial (r < 92) is left free for the note name

// Detected note, big, in the dial (DejaVu40: capitals 29 px tall).
constexpr int kBigBaseline = 121;         // y of the baseline
constexpr int kBigSharpH = 30;            // sharp sign height next to the big note
constexpr int kBigSharpGap = 2;           // free space on each side of the sharp sign

const char* kNoteLetters[] = {"C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B"};
const bool kNoteSharp[] = {false, true, false, true, false, false, true, false, true, false, true, false};

float noteFreq(int midi, float refA) { return refA * powf(2.0f, (midi - 69) / 12.0f); }

float polarX(float r, float deg) { return kPivotX + r * sinf(deg * (float)M_PI / 180.0f); }
float polarY(float r, float deg) { return kPivotY - r * cosf(deg * (float)M_PI / 180.0f); }

// Width of drawSharp() for a given height.
int sharpWidth(int h) {
    const int t = h >= 24 ? 3 : 2;
    return 4 + 2 * t + (h * 3 + 10) / 18;
}

// Musical sharp sign, h px tall, top-left at (x, y); sharpWidth(h) wide (11 px at h = 18).
// Two vertical stems (the right one 2 px higher than the left one) crossed by two bars rising to
// the right. Stems and bars are t px thick (2 up to h = 23, 3 above), the bars stick out 2 px on
// each side of the stems and there is clear space between the stems and between the bars.
// Everything is axis-aligned rectangles (the bars are drawn one column at a time), so it is
// crisp at any size.
void drawSharp(M5Canvas& c, int x, int y, int h, uint16_t col) {
    const int t = h >= 24 ? 3 : 2;
    const int ext = 2;
    const int w = sharpWidth(h);
    const int rise = (h * 3 + 9) / 18;  // how much the bars climb from the left end to the right end
    c.fillRect(x + ext, y + 2, t, h - 2, col);
    c.fillRect(x + w - ext - t, y, t, h - 2, col);
    const int mid[2] = {h * 34 / 100, h * 68 / 100};  // vertical centre of each bar
    for (int m : mid) {
        for (int i = 0; i < w; ++i) {
            const int dy = (int)floorf(rise * (float)(w - 1 - 2 * i) / (2.0f * (w - 1)) + 0.5f);
            c.fillRect(x + i, y + m - t / 2 + dy, 1, t, col);
        }
    }
}

// "< 4G# >" selector, centred on y. Octave first, then the note, then the
// sharp: each in a fixed-width cell so the arrows never move.
void drawNoteSelector(M5Canvas& c, int midi, int cy) {
    const int x0 = 16;
    c.setFont(&fonts::Font2);
    c.setTextDatum(middle_center);
    c.setTextColor(CYAN);
    c.drawString("<", 7, cy);
    c.drawString(">", x0 + 56, cy);

    c.setFont(&fonts::Font4);
    c.setTextDatum(middle_center);
    c.setTextColor(WHITE);
    c.drawString(String(midi / 12 - 1), x0 + 8, cy);
    c.drawString(kNoteLetters[midi % 12], x0 + 26, cy);
    if (kNoteSharp[midi % 12]) drawSharp(c, x0 + 36, cy - 10, 18, WHITE);
}

// Detected note, e.g. "A#4", centred on cx, baseline at kBigBaseline.
void drawBigNote(M5Canvas& c, int midi, int cx, uint16_t col) {
    const int pc = midi % 12;
    const bool sharp = kNoteSharp[pc];
    const String oct = String(midi / 12 - 1);
    c.setFont(&fonts::DejaVu40);
    c.setTextSize(1);
    c.setTextDatum(baseline_left);
    c.setTextColor(col);
    const int wl = c.textWidth(kNoteLetters[pc]);
    const int wo = c.textWidth(oct);
    const int ws = sharp ? sharpWidth(kBigSharpH) + 2 * kBigSharpGap : 0;
    int x = cx - (wl + ws + wo) / 2;
    c.drawString(kNoteLetters[pc], x, kBigBaseline);
    x += wl;
    if (sharp) {
        drawSharp(c, x + kBigSharpGap, kBigBaseline - (kBigSharpH - 1), kBigSharpH, col);
        x += ws;
    }
    c.drawString(oct, x, kBigBaseline);
}

class TunerMode : public Mode {
public:
    char hotkey() const override { return 't'; }
    const char* label() const override { return "Tuner"; }

    // The tuner has two views: the classic needle and the ToneMap heat map (tonemap_view.cpp).
    // The last one used is persisted (settings tunerView) and opened on entry.
    void enter() override {
        tm_ = settings::get().tunerView == 1;
        if (tm_) {
            tonemap::enter();
        } else {
            enterClassic();
        }
    }

    void leave() override {
        if (tm_) {
            tonemap::leave();
        } else {
            leaveClassic();
        }
    }

    // T pressed while the tuner is already active: switch view.
    void onReselect() override {
        auto& s = settings::get();
        if (tm_) {
            tonemap::leave();
            tm_ = false;
            s.tunerView = 0;
            settings::touch();
            enterClassic();
        } else {
            leaveClassic();
            tm_ = true;
            s.tunerView = 1;
            settings::touch();
            tonemap::enter();
        }
        dirty_ = true;
    }

    bool acceptsAlt() const override { return tm_; }  // Alt opens the ToneMap options overlay

    void afterPush() override {
        if (tm_) tonemap::afterPush();
    }

    void onKey(const KeyEvent& e) override {
        if (tm_) return tonemap::onKey(e);
        auto& s = settings::get();
        switch (e.key) {
            case Key::Left:
                s.targetNote = constrain(s.targetNote - 1, settings::kMinNote, settings::kMaxNote);
                droneNoteChanged();
                break;
            case Key::Right:
                s.targetNote = constrain(s.targetNote + 1, settings::kMinNote, settings::kMaxNote);
                droneNoteChanged();
                break;
            case Key::Up:
                s.refA = constrain(s.refA + 1.0f, settings::kMinRefA, settings::kMaxRefA);
                refAChanged();
                break;
            case Key::Down:
                s.refA = constrain(s.refA - 1.0f, settings::kMinRefA, settings::kMaxRefA);
                refAChanged();
                break;
            case Key::Space:
                toggleDrone();
                break;
            case Key::Char:
                if (e.ch == 'p') toggleDrone();
                if (e.ch == 'l') toggleLed();
#ifdef TUNER_DEBUG
                if (e.ch == 'b' || e.ch == 'q' || e.ch == 's') {
                    stopCapture();  // the sound experiments need the speaker
                    hasReading_ = false;
                    audio::debugTone(e.ch, noteFreq(s.targetNote, s.refA));
                }
#endif
                break;
            default:
                break;
        }
        dirty_ = true;
    }

    void update() override {
        if (tm_) return tonemap::update();
        const uint32_t now = millis();
        pollCapture(now);
#ifdef TUNER_DEBUG
        if (now - dbgRefreshMs_ > 250) {  // keep the codec register readout live
            dbgRefreshMs_ = now;
            dirty_ = true;
        }
#endif

        // Needle: exponential approach (time constant ~70 ms).
        const float dt = (now - lastUpdateMs_) / 1000.0f;
        lastUpdateMs_ = now;
        if (hasReading_ && now - lastReadingMs_ > kHoldMs) {
            hasReading_ = false;
            dirty_ = true;
        }
        targetDeg_ = hasReading_ ? constrain(cents_, -kFullScale, kFullScale) : kRestDeg;
        needleDeg_ += (targetDeg_ - needleDeg_) * (1.0f - expf(-dt / 0.07f));
        if (fabsf(needleDeg_ - drawnDeg_) > 0.15f) dirty_ = true;

        if (footerMsg_[0] != 0 && (int32_t)(now - footerMsgUntil_) >= 0) {
            footerMsg_[0] = 0;
            dirty_ = true;
        }
        updateLed();
    }

    bool draw(M5Canvas& c, bool force) override {
        if (tm_) return tonemap::draw(c, force);
        const uint32_t now = millis();
        if (!force && (!dirty_ || now - lastDrawMs_ < 30)) return false;
        dirty_ = false;
        lastDrawMs_ = now;
        drawnDeg_ = needleDeg_;

        const auto& s = settings::get();
        const bool drone = audio::droneOn();
        const bool shown = hasReading_ && !drone;
        c.fillSprite(BLACK);

        // Gauge band, green zone, ticks and labels (all grey while the drone plays)
        const uint16_t bandCol = c.color565(40, 40, 40);
        const uint16_t zoneCol = drone ? c.color565(75, 75, 75) : (uint16_t)GREEN;
        const uint16_t tickMajor = drone ? c.color565(90, 90, 90) : (uint16_t)WHITE;
        const uint16_t tickMinor = drone ? c.color565(60, 60, 60) : (uint16_t)LIGHTGREY;
        c.fillArc(kPivotX, kPivotY, kBandInner, kBandOuter, 270 - kFullScale, 270 + kFullScale, bandCol);
        c.fillArc(kPivotX, kPivotY, kBandInner, kBandOuter, 270 - kGreenCents, 270 + kGreenCents, zoneCol);
        for (int cents = -50; cents <= 50; cents += 5) {
            const bool major = (cents % 25) == 0;
            const float r1 = kBandOuter + (major ? 9 : 5);
            c.drawLine(polarX(kBandOuter, cents), polarY(kBandOuter, cents), polarX(r1, cents), polarY(r1, cents),
                       major ? tickMajor : tickMinor);
        }
        c.setFont(&fonts::Font0);
        c.setTextSize(1);
        c.setTextDatum(middle_center);
        c.setTextColor(drone ? c.color565(70, 70, 70) : (uint16_t)LIGHTGREY);
        static const int labelCents[] = {-50, -25, 0, 25, 50};
        static const char* labelText[] = {"-50", "-25", "0", "+25", "+50"};
        for (int i = 0; i < 5; ++i) {
            c.drawString(labelText[i], polarX(kBandInner - 9, labelCents[i]), polarY(kBandInner - 9, labelCents[i]));
        }

        // Needle (starts at the inside edge of the label ring: the dial's inside is for the note)
        const bool inTune = inTuneNow();
        const uint16_t needleCol = (drone || !hasReading_) ? c.color565(90, 90, 90) : (inTune ? GREEN : RED);
        c.drawWedgeLine(polarX(kNeedleInner, needleDeg_), polarY(kNeedleInner, needleDeg_),
                        polarX(kBandOuter + 4, needleDeg_), polarY(kBandOuter + 4, needleDeg_), 3.0f, 1.0f, needleCol);

        // Dial centre: the detected note, big (dim "--" while nothing is heard, a notice while the mic is off)
        if (drone) {
            c.setFont(&fonts::Font0);
            c.setTextSize(1);
            c.setTextDatum(middle_center);
            c.setTextColor(c.color565(110, 110, 110));
            c.drawString("Mic off", 120, 100);
            c.drawString("(drone playing)", 120, 111);
        } else if (shown) {
            drawBigNote(c, midi_, 120, inTune ? (uint16_t)GREEN : (uint16_t)WHITE);
        } else if (!micFailed_) {
            c.setFont(&fonts::DejaVu40);
            c.setTextSize(1);
            c.setTextDatum(baseline_center);
            c.setTextColor(c.color565(60, 60, 60));
            c.drawString("--", 120, kBigBaseline);
        }

        // Header: drone note selector (and its frequency), detected cents and frequency in the middle,
        // the A reference on the right
        const float target = noteFreq(s.targetNote, s.refA);
        drawNoteSelector(c, s.targetNote, 15);
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(drone ? (uint16_t)CYAN : (uint16_t)LIGHTGREY);
        c.drawString("Drone " + String(target, 1) + " Hz", 4, 32);

        if (shown) {
            const int ci = (int)lroundf(cents_);
            char buf[16];
            if (ci == 0) {
                snprintf(buf, sizeof(buf), "0c");
            } else {
                snprintf(buf, sizeof(buf), "%+dc", ci);
            }
            c.setFont(&fonts::Font4);
            c.setTextSize(1);
            c.setTextDatum(top_center);
            c.setTextColor(inTune ? (uint16_t)GREEN : (uint16_t)WHITE);
            c.drawString(buf, 118, 1);
            snprintf(buf, sizeof(buf), "%.1f Hz", freq_);
            c.setFont(&fonts::Font0);
            c.setTextColor(LIGHTGREY);
            c.drawString(buf, 118, 28);
        }
        shownMidi_ = shown ? midi_ : -1;
        shownCents_ = shown ? (int)lroundf(cents_) : 0;
        shownHzTenths_ = shown ? (int)lroundf(freq_ * 10.0f) : 0;

        const String ref = "A=" + String(s.refA, 0) + " Hz";
        c.setFont(&fonts::Font2);
        c.setTextDatum(top_right);
        c.setTextColor(YELLOW);
        c.drawString(ref, 236, 2);
        ui::drawUpDown(c, 236 - c.textWidth(ref) - 9, 2, YELLOW);

        // Footer: microphone error on the left, drone key hint on the right
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        if (micFailed_ && !drone) {
            c.setTextColor(RED);
            c.drawString("Microphone unavailable", 4, 115);
        }
        c.setTextDatum(top_right);
        if (footerMsg_[0] != 0) {
            c.setTextColor(WHITE);
            c.drawString(footerMsg_, 236, 115);
        } else {
            c.setTextColor(drone ? CYAN : DARKGREY);
            c.drawString(drone ? "Space: stop drone" : "Space: drone", 236, 115);
        }

#ifdef TUNER_DEBUG
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(ORANGE);
        char dbg[48];
        dbgRecFails_ = tonemap_audio::stats().recFail;
        snprintf(dbg, sizeof(dbg), "frames=%u recfail=%u", (unsigned)dbgFrames_, (unsigned)dbgRecFails_);
        c.drawString(dbg, 4, 44);
        snprintf(dbg, sizeof(dbg), "rms=%.0f peak=%.0f", dbgRms_, dbgPeak_);
        c.drawString(dbg, 4, 53);
        snprintf(dbg, sizeof(dbg), "valid=%d f=%.1f clar=%.2f", (int)dbgValid_, dbgFreq_, dbgClarity_);
        c.drawString(dbg, 4, 62);
        snprintf(dbg, sizeof(dbg), "mic=%d spk=%d play=%d drone=%d", (int)audio::micOn(),
                 (int)M5Cardputer.Speaker.isRunning(), (int)M5Cardputer.Speaker.isPlaying(), (int)drone);
        c.drawString(dbg, 4, 71);
        c.setTextColor(YELLOW);
        c.drawString(audio::debugStatus(), 4, 80);
        c.setTextColor(YELLOW);
        c.drawString(audio::debugCodec(), 4, 89);
        c.setTextColor(DARKGREY);
        c.drawString("B beep  Q square  S sine", 4, 98);
#endif
        return true;
    }

private:
    // ---- microphone / capture (same entry and exit discipline as tonemap::enter / leave) ----
    // The 32 kHz capture needs, in this order: attach() with the mic stopped, audio::micStart(), start().
    // Leaving: stop(), audio::micStop() (waits for the chunks in flight, brings the speaker back), detach().
    void startCapture() {
        if (audio::micOn()) audio::micStop();  // attach() needs the mic stopped
        tonemap_audio::attach();
        autoGate_.reset();
        capture_ = true;
        listening_ = audio::micStart() && tonemap_audio::start();
        micFailed_ = !listening_;
        if (micFailed_) Serial.println("[tuner] microphone unavailable");
    }

    void stopCapture() {
        if (!capture_) return;
        tonemap_audio::stop();
        audio::micStop();
        tonemap_audio::detach();  // back to the 16 kHz default
        autoGate_.reset();
        capture_ = false;
        listening_ = false;
    }

    void resetReading() {
        hasReading_ = false;
        midi_ = -1;
    }

    void enterClassic() {
        resetReading();
        capture_ = false;
        footerMsg_[0] = 0;
        tuner_led::reset();
        if (settings::get().tmLed != 0) tuner_led::brightnessRaise();
        startCapture();
        needleDeg_ = kRestDeg;
        targetDeg_ = kRestDeg;
        lastUpdateMs_ = millis();
        dirty_ = true;
    }

    void leaveClassic() {
        audio::droneStop();  // fade-out; the mic is already off while the drone plays
        tuner_led::offBlocking();  // while the backlight pin is still held at full brightness
        tuner_led::brightnessRestore();
        footerMsg_[0] = 0;
        stopCapture();
        resetReading();
    }

    // In tune right now, as the screen shows it: a reading is displayed (not while the drone plays) and the
    // smoothed deviation is inside the green band. Needle colour, big note and LED all use this.
    bool inTuneNow() const { return hasReading_ && !audio::droneOn() && fabsf(cents_) <= kGreenCents; }

    // Called every loop in the classic view: green while in tune, otherwise off; sends the pending repeats.
    void updateLed() {
        const bool want = settings::get().tmLed != 0 && listening_ && inTuneNow();
        if (want != tuner_led::lit()) {
            tuner_led::setLit(want);
            if (want) {
                tuner_led::show(0, 255, 0);  // same green as the metronome beats
            } else {
                tuner_led::show(0, 0, 0);
            }
        }
        tuner_led::service();
    }

    void toggleLed() {
        auto& s = settings::get();
        s.tmLed = s.tmLed ? 0 : 1;
        settings::touch();
        if (s.tmLed) {
            tuner_led::brightnessRaise();
        } else {
            tuner_led::offBlocking();
            tuner_led::brightnessRestore();
        }
        showFooterMsg(s.tmLed ? "LED: on" : "LED: off");
        Serial.printf("[tuner] LED %s\n", s.tmLed ? "on" : "off");
    }

    void showFooterMsg(const char* text) {
        strlcpy(footerMsg_, text, sizeof(footerMsg_));
        footerMsgUntil_ = millis() + kFooterMsgMs;
    }

    void droneNoteChanged() {
        settings::touch();
        if (audio::droneOn()) {
            const auto& s = settings::get();
            audio::droneStart(noteFreq(s.targetNote, s.refA));
        }
    }

    void refAChanged() {
        resetReading();  // the cents of the last reading were relative to the old reference
        droneNoteChanged();
    }

    void toggleDrone() {
        const auto& s = settings::get();
        if (audio::droneOn()) {
            audio::droneStop();
            startCapture();
            resetReading();
            Serial.println("[tuner] drone off");
        } else {
            stopCapture();
            resetReading();
            const float f = noteFreq(s.targetNote, s.refA);
            audio::droneStart(f);
            Serial.printf("[tuner] drone on, note %d, %.2f Hz\n", s.targetNote, f);
        }
    }

    // ---- detection ----
    void pollCapture(uint32_t now) {
        if (!listening_ || audio::droneOn()) return;
        tonemap_audio::service();
        tonemap_audio::WindowInfo wi;
        if (tonemap_audio::takeWindow(tonemap_detect::window(), wi)) processWindow(wi, now);
    }

    void processWindow(const tonemap_audio::WindowInfo& wi, uint32_t now) {
        const float refA = settings::get().refA;
        // Every window goes through prepare() and updates the automatic gate (also those below it, so that the floor
        // can fall in silence); findPitch only runs above the threshold.
        const tonemap_detect::Analysis an = tonemap_detect::analyse(autoGate_, wi.dtMs);

        // Nearest note over the whole range (written = concert pitch, no transposition).
        tonemap::NoteHit hit = {-1, 0.0f, 0.0f, 0.0f};
        if (an.valid) hit = tonemap::freqToNote(an.hz, refA);
        const bool accepted = hit.concertMidi >= tonemap::kGridLo && hit.concertMidi <= tonemap::kGridHi;

#ifdef TUNER_DEBUG
        ++dbgFrames_;
        dbgRms_ = an.rms;
        dbgPeak_ = an.peak;
        dbgValid_ = an.valid;
        dbgFreq_ = an.hz;
        dbgClarity_ = an.clarity;
        dirty_ = true;
#endif

        if (accepted) {
            // Light smoothing between windows of the same note; a new note or a large jump is taken at once.
            if (hasReading_ && hit.concertMidi == midi_ && wi.dtMs > 0 && fabsf(hit.cents - cents_) < kJumpCents) {
                cents_ += (hit.cents - cents_) * (1.0f - expf(-(float)wi.dtMs / kSmoothTauMs));
            } else {
                cents_ = hit.cents;
            }
            midi_ = hit.concertMidi;
            freq_ = hit.targetHz * powf(2.0f, cents_ / 1200.0f);
            hasReading_ = true;
            lastReadingMs_ = now;
            if (midi_ != shownMidi_ || (int)lroundf(cents_) != shownCents_ ||
                (int)lroundf(freq_ * 10.0f) != shownHzTenths_) {
                dirty_ = true;
            }
        }

        if (now - lastLogMs_ > 500) {
            lastLogMs_ = now;
            Serial.printf("[tuner] rms=%.0f gate=%.0f peak=%.0f valid=%d f=%.2f clarity=%.2f -> %s", an.rms, an.gate,
                          an.peak, (int)an.valid, an.hz, an.clarity, accepted ? "ok" : "ignored");
            if (accepted) {
                Serial.printf(" note=%s%d%s %+.1f cents\n", kNoteLetters[midi_ % 12], midi_ / 12 - 1,
                              kNoteSharp[midi_ % 12] ? "#" : "", cents_);
            } else {
                Serial.println();
            }
        }
    }

    bool tm_ = false;         // ToneMap view active
    bool capture_ = false;    // tonemap_audio attached (needs stopCapture() on exit)
    bool listening_ = false;  // microphone on and capture running
    bool micFailed_ = false;  // the last startCapture() failed
    bool hasReading_ = false;
    tonemap_detect::AutoGate autoGate_;  // reset with the capture (enter / leave / drone on-off), not per lost reading
    int midi_ = -1;           // nearest note of the last reading
    float cents_ = 0;         // smoothed, relative to midi_
    float freq_ = 0;
    int shownMidi_ = -1;      // what the last drawn frame showed
    int shownCents_ = 0;
    int shownHzTenths_ = 0;
    float needleDeg_ = kRestDeg;
    float targetDeg_ = kRestDeg;
    float drawnDeg_ = 999;
    uint32_t lastReadingMs_ = 0;
    uint32_t lastUpdateMs_ = 0;
    uint32_t lastDrawMs_ = 0;
    uint32_t lastLogMs_ = 0;
    bool dirty_ = true;
    char footerMsg_[16] = "";  // transient footer text ("LED: on"); empty = none
    uint32_t footerMsgUntil_ = 0;
#ifdef TUNER_DEBUG
    uint32_t dbgFrames_ = 0, dbgRecFails_ = 0, dbgRefreshMs_ = 0;
    float dbgRms_ = 0, dbgPeak_ = 0, dbgFreq_ = 0, dbgClarity_ = 0;
    bool dbgValid_ = false;
#endif
};

}  // namespace

Mode& tunerMode() {
    static TunerMode mode;
    return mode;
}
