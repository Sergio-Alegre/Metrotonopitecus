// Tuner (key T).
//   Left / Right  target note, one semitone per step (C3..B5)
//   Up / Down     reference A4 in Hz (default 442, persisted)
//   Space or P    sine drone on the target note (pauses detection)
//
// The needle shows how far the detected pitch is from the target note, on an
// arc of about 100 degrees like an analog meter: centre = in tune, green band
// = +-5 cents, full scale = +-50 cents.
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

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

float noteFreq(int midi, float refA) { return refA * powf(2.0f, (midi - 69) / 12.0f); }

String noteName(int midi) { return String(kNoteNames[midi % 12]) + String(midi / 12 - 1); }

float polarX(float r, float deg) { return kPivotX + r * sinf(deg * (float)M_PI / 180.0f); }
float polarY(float r, float deg) { return kPivotY - r * cosf(deg * (float)M_PI / 180.0f); }

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
                break;
            default:
                break;
        }
        dirty_ = true;
    }

    void update() override {
        const uint32_t now = millis();
        pollMic(now);

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
        c.fillSprite(BLACK);

        // Gauge band, green zone, ticks and labels
        const uint16_t bandCol = c.color565(40, 40, 40);
        c.fillArc(kPivotX, kPivotY, kBandInner, kBandOuter, 270 - kFullScale, 270 + kFullScale, bandCol);
        c.fillArc(kPivotX, kPivotY, kBandInner, kBandOuter, 270 - kGreenCents, 270 + kGreenCents, GREEN);
        for (int cents = -50; cents <= 50; cents += 5) {
            const bool major = (cents % 25) == 0;
            const float r1 = kBandOuter + (major ? 9 : 5);
            c.drawLine(polarX(kBandOuter, cents), polarY(kBandOuter, cents), polarX(r1, cents), polarY(r1, cents),
                       major ? WHITE : LIGHTGREY);
        }
        c.setFont(&fonts::Font0);
        c.setTextSize(1);
        c.setTextDatum(middle_center);
        c.setTextColor(LIGHTGREY);
        static const int labelCents[] = {-50, -25, 0, 25, 50};
        static const char* labelText[] = {"-50", "-25", "0", "+25", "+50"};
        for (int i = 0; i < 5; ++i) {
            c.drawString(labelText[i], polarX(kBandInner - 9, labelCents[i]), polarY(kBandInner - 9, labelCents[i]));
        }

        // Needle
        const bool inTune = hasReading_ && fabsf(cents_) <= kGreenCents;
        const uint16_t needleCol = !hasReading_ ? c.color565(90, 90, 90) : (inTune ? GREEN : RED);
        c.drawWedgeLine(polarX(46, needleDeg_), polarY(46, needleDeg_), polarX(kBandOuter + 4, needleDeg_),
                        polarY(kBandOuter + 4, needleDeg_), 3.0f, 1.0f, needleCol);

        // Header: target note, its frequency and the A reference
        const float target = noteFreq(s.targetNote, s.refA);
        c.setFont(&fonts::Font4);
        c.setTextDatum(top_left);
        c.setTextColor(WHITE);
        c.drawString(noteName(s.targetNote), 4, 2);
        c.setFont(&fonts::Font0);
        c.setTextColor(LIGHTGREY);
        c.drawString(String(target, 1) + " Hz", 4, 30);

        c.setFont(&fonts::Font2);
        c.setTextDatum(top_right);
        c.setTextColor(YELLOW);
        c.drawString("A=" + String(s.refA, 0) + " Hz", 236, 2);

        // Footer: reading and drone state
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        if (audio::droneOn()) {
            c.setTextColor(CYAN);
            c.drawString("Drone playing - mic paused", 4, 115);
        } else if (!micOk_) {
            c.setTextColor(RED);
            c.drawString("Microphone unavailable", 4, 115);
        } else if (hasReading_) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.1f Hz  %+.0f cents", freq_, cents_);
            c.setTextColor(inTune ? GREEN : WHITE);
            c.drawString(buf, 4, 115);
        } else {
            c.setTextColor(DARKGREY);
            c.drawString("Listening...", 4, 115);
        }
        c.setTextDatum(top_right);
        c.setTextColor(audio::droneOn() ? CYAN : DARKGREY);
        c.drawString(audio::droneOn() ? "Space: stop drone" : "Space: drone", 236, 115);
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
            Serial.printf("[tuner] drone on: %s %.2f Hz\n", noteName(s.targetNote).c_str(), f);
        }
    }

    void pollMic(uint32_t now) {
        if (!audio::micOn() || audio::droneOn()) return;
        if (M5Cardputer.Mic.isRecording()) return;

        if (pending_) {
            pending_ = false;
            process(now);
        }
        if (M5Cardputer.Mic.record(micBuf, kFrame, (uint32_t)kMicRate)) pending_ = true;
    }

    void process(uint32_t now) {
        const auto& s = settings::get();
        const float target = noteFreq(s.targetNote, s.refA);
        const pitch::Result r = pitch::detect(micBuf, kFrame, kMicRate, target, kSearchSpan, workBuf);

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
            Serial.printf("[tuner] rms=%.0f valid=%d f=%.2f clarity=%.2f -> %s cents=%.1f\n", r.rms, (int)r.valid, r.freq,
                          r.clarity, accepted ? "ok" : "ignored", accepted ? cents_ : 0.0f);
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
};

}  // namespace

Mode& tunerMode() {
    static TunerMode mode;
    return mode;
}
