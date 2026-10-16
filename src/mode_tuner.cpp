// Tuner (key T).
//   Left / Right  target note, one semitone per step (C3..B5)
//   Up / Down     reference A4 in Hz (default 442, persisted)
//   Space or P    sine drone on the target note (pauses detection)
//
// The needle shows how far the detected pitch is from the target note, on an
// arc of about 100 degrees like an analog meter: centre = in tune, green band
// = +-5 cents, full scale = +-50 cents. While the drone plays the tuner is not
// listening, and the whole gauge turns grey to show it.
//
// The ADV microphone and speaker share clock pins, so detection is paused
// while the drone plays (and the drone would be what the mic hears anyway).
#include <math.h>

#include "audio.h"
#include "modes.h"
#include "pitch.h"
#include "settings.h"

namespace {

constexpr float kMicRate = 16000.0f;
constexpr size_t kFrame = 2048;
constexpr float kSearchSpan = 2.0f;     // semitones around the target
constexpr float kMinRms = 120.0f;       // below this the input is treated as silence
constexpr float kMinClarity = 0.75f;
constexpr float kAcceptCents = 100.0f;  // ignore readings further than this from the target
constexpr uint32_t kHoldMs = 700;       // keep the last reading this long without new ones
constexpr float kFullScale = 50.0f;     // cents at the ends of the arc
constexpr float kGreenCents = 5.0f;
constexpr float kRestDeg = -52.0f;      // needle parked just past the left end

// Gauge geometry: pivot below the visible area so only a slice of the arc shows.
constexpr int kPivotX = 120;
constexpr int kPivotY = 175;
constexpr int kBandInner = 112;
constexpr int kBandOuter = 130;

int16_t micBuf[kFrame];
float workBuf[kFrame];

const char* kNoteLetters[] = {"C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B"};
const bool kNoteSharp[] = {false, true, false, true, false, false, true, false, true, false, true, false};

float noteFreq(int midi, float refA) { return refA * powf(2.0f, (midi - 69) / 12.0f); }

float polarX(float r, float deg) { return kPivotX + r * sinf(deg * (float)M_PI / 180.0f); }
float polarY(float r, float deg) { return kPivotY - r * cosf(deg * (float)M_PI / 180.0f); }

// Musical sharp sign: two vertical stems (the right one higher) crossed by two
// thick bars rising to the right. 11 px wide, h px tall, top-left at (x, y).
void drawSharp(M5Canvas& c, int x, int y, int h, uint16_t col) {
    c.fillRect(x + 2, y + 2, 2, h - 2, col);
    c.fillRect(x + 7, y, 2, h - 2, col);
    const int w = 11, rise = 3, t = 4;
    const int bars[2] = {y + h * 38 / 100 + 2, y + h * 70 / 100 + 1};
    for (int yt : bars) {
        c.fillTriangle(x, yt, x + w, yt - rise, x + w, yt - rise + t, col);
        c.fillTriangle(x, yt, x + w, yt - rise + t, x, yt + t, col);
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
    c.setTextColor(WHITE);
    c.drawString(String(midi / 12 - 1), x0 + 8, cy);
    c.drawString(kNoteLetters[midi % 12], x0 + 26, cy);
    if (kNoteSharp[midi % 12]) drawSharp(c, x0 + 36, cy - 10, 18, WHITE);
}

class TunerMode : public Mode {
public:
    char hotkey() const override { return 't'; }
    const char* label() const override { return "Tuner"; }

    void enter() override {
        micOk_ = audio::micStart();
        pending_ = false;
        hasReading_ = false;
        needleDeg_ = kRestDeg;
        targetDeg_ = kRestDeg;
        lastUpdateMs_ = millis();
        dirty_ = true;
    }

    void leave() override {
        audio::droneStop();
        audio::micStop();
        pending_ = false;
    }

    void onKey(const KeyEvent& e) override {
        auto& s = settings::get();
        switch (e.key) {
            case Key::Left:
                s.targetNote = constrain(s.targetNote - 1, settings::kMinNote, settings::kMaxNote);
                targetChanged();
                break;
            case Key::Right:
                s.targetNote = constrain(s.targetNote + 1, settings::kMinNote, settings::kMaxNote);
                targetChanged();
                break;
            case Key::Up:
                s.refA = constrain(s.refA + 1.0f, settings::kMinRefA, settings::kMaxRefA);
                targetChanged();
                break;
            case Key::Down:
                s.refA = constrain(s.refA - 1.0f, settings::kMinRefA, settings::kMaxRefA);
                targetChanged();
                break;
            case Key::Space:
                toggleDrone();
                break;
            case Key::Char:
                if (e.ch == 'p') toggleDrone();
#ifdef TUNER_DEBUG
                if (e.ch == 'b' || e.ch == 'q' || e.ch == 's') {
                    pending_ = false;
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
        const uint32_t now = millis();
        pollMic(now);
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
    }

    bool draw(M5Canvas& c, bool force) override {
        const uint32_t now = millis();
        if (!force && (!dirty_ || now - lastDrawMs_ < 30)) return false;
        dirty_ = false;
        lastDrawMs_ = now;
        drawnDeg_ = needleDeg_;

        const auto& s = settings::get();
        const bool drone = audio::droneOn();
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

        // Needle
        const bool inTune = !drone && hasReading_ && fabsf(cents_) <= kGreenCents;
        const uint16_t needleCol = (drone || !hasReading_) ? c.color565(90, 90, 90) : (inTune ? GREEN : RED);
        c.drawWedgeLine(polarX(46, needleDeg_), polarY(46, needleDeg_), polarX(kBandOuter + 4, needleDeg_),
                        polarY(kBandOuter + 4, needleDeg_), 3.0f, 1.0f, needleCol);

        // Header: target note selector, its frequency, and the A reference
        const float target = noteFreq(s.targetNote, s.refA);
        drawNoteSelector(c, s.targetNote, 15);
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(LIGHTGREY);
        c.drawString(String(target, 1) + " Hz", 4, 32);

        const String ref = "A=" + String(s.refA, 0) + " Hz";
        c.setFont(&fonts::Font2);
        c.setTextDatum(top_right);
        c.setTextColor(YELLOW);
        c.drawString(ref, 236, 2);
        ui::drawUpDown(c, 236 - c.textWidth(ref) - 9, 2, YELLOW);

        // Footer: reading (or errors) on the left, drone key hint on the right
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        if (!micOk_ && !drone) {
            c.setTextColor(RED);
            c.drawString("Microphone unavailable", 4, 115);
        } else if (hasReading_ && !drone) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.1f Hz  %+.0f cents", freq_, cents_);
            c.setTextColor(inTune ? GREEN : WHITE);
            c.drawString(buf, 4, 115);
        }
        c.setTextDatum(top_right);
        c.setTextColor(drone ? CYAN : DARKGREY);
        c.drawString(drone ? "Space: stop drone" : "Space: drone", 236, 115);

#ifdef TUNER_DEBUG
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(ORANGE);
        char dbg[48];
        snprintf(dbg, sizeof(dbg), "frames=%u recfail=%u", (unsigned)dbgFrames_, (unsigned)dbgRecFails_);
        c.drawString(dbg, 4, 44);
        snprintf(dbg, sizeof(dbg), "rms=%.0f peak=%d", dbgRms_, dbgPeak_);
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
    void targetChanged() {
        settings::touch();
        hasReading_ = false;
        if (audio::droneOn()) {
            const auto& s = settings::get();
            audio::droneStart(noteFreq(s.targetNote, s.refA));
        }
    }

    void toggleDrone() {
        const auto& s = settings::get();
        if (audio::droneOn()) {
            audio::droneStop();
            micOk_ = audio::micStart();
            pending_ = false;
            Serial.println("[tuner] drone off");
        } else {
            audio::micStop();
            pending_ = false;
            hasReading_ = false;
            const float f = noteFreq(s.targetNote, s.refA);
            audio::droneStart(f);
            Serial.printf("[tuner] drone on, note %d, %.2f Hz\n", s.targetNote, f);
        }
    }

    void pollMic(uint32_t now) {
        if (!audio::micOn() || audio::droneOn()) return;
        if (M5Cardputer.Mic.isRecording()) return;

        if (pending_) {
            pending_ = false;
            process(now);
        }
        if (M5Cardputer.Mic.record(micBuf, kFrame, (uint32_t)kMicRate)) {
            pending_ = true;
        } else {
#ifdef TUNER_DEBUG
            ++dbgRecFails_;
            dirty_ = true;
#endif
            if (now - lastLogMs_ > 1000) {
                lastLogMs_ = now;
                Serial.println("[tuner] Mic.record() refused the request");
            }
        }
    }

    void process(uint32_t now) {
        const auto& s = settings::get();
        const float target = noteFreq(s.targetNote, s.refA);
        const pitch::Result r = pitch::detect(micBuf, kFrame, kMicRate, target, kSearchSpan, workBuf);

#ifdef TUNER_DEBUG
        ++dbgFrames_;
        dbgRms_ = r.rms;
        dbgValid_ = r.valid;
        dbgFreq_ = r.freq;
        dbgClarity_ = r.clarity;
        dbgPeak_ = 0;
        for (size_t i = 0; i < kFrame; ++i) dbgPeak_ = max(dbgPeak_, abs((int)micBuf[i]));
        dirty_ = true;
#endif
        const bool good = r.valid && r.rms >= kMinRms && r.clarity >= kMinClarity;
        float cents = 0;
        bool accepted = false;
        if (good) {
            cents = 1200.0f * log2f(r.freq / target);
            accepted = fabsf(cents) <= kAcceptCents;
        }

        if (accepted) {
            // Light smoothing between frames; large jumps are taken immediately.
            if (hasReading_ && fabsf(cents - cents_) < 25.0f) {
                cents_ += 0.5f * (cents - cents_);
            } else {
                cents_ = cents;
            }
            freq_ = target * powf(2.0f, cents_ / 1200.0f);
            hasReading_ = true;
            lastReadingMs_ = now;
            dirty_ = true;
        }

        if (now - lastLogMs_ > 500) {
            lastLogMs_ = now;
            int peak = 0;
            for (size_t i = 0; i < kFrame; ++i) peak = max(peak, abs((int)micBuf[i]));
            Serial.printf("[tuner] peak=%d rms=%.0f valid=%d f=%.2f clarity=%.2f target=%.2f -> %s cents=%.1f\n", peak,
                          r.rms, (int)r.valid, r.freq, r.clarity, target, accepted ? "ok" : "ignored",
                          accepted ? cents_ : 0.0f);
        }
    }

    bool micOk_ = false;
    bool pending_ = false;
    bool hasReading_ = false;
    float cents_ = 0;
    float freq_ = 0;
    float needleDeg_ = kRestDeg;
    float targetDeg_ = kRestDeg;
    float drawnDeg_ = 999;
    uint32_t lastReadingMs_ = 0;
    uint32_t lastUpdateMs_ = 0;
    uint32_t lastDrawMs_ = 0;
    uint32_t lastLogMs_ = 0;
    bool dirty_ = true;
#ifdef TUNER_DEBUG
    uint32_t dbgFrames_ = 0, dbgRecFails_ = 0, dbgRefreshMs_ = 0;
    float dbgRms_ = 0, dbgFreq_ = 0, dbgClarity_ = 0;
    int dbgPeak_ = 0;
    bool dbgValid_ = false;
#endif
};

}  // namespace

Mode& tunerMode() {
    static TunerMode mode;
    return mode;
}
