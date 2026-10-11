#include "audio.h"

#include <M5Cardputer.h>

#include <math.h>

#include "settings.h"

namespace {

constexpr uint8_t kDroneChannel = 0;
constexpr uint8_t kClickChannel = 1;
// One sine cycle, 8-bit unsigned. The speaker reads it at freq * kSineLen Hz,
// which must stay below its 48 kHz output rate (988 Hz * 48 = 47.4 kHz); the
// proven working range of the M5Unified resampler. Linear interpolation of 48
// points per cycle keeps distortion under -50 dB.
constexpr size_t kSineLen = 48;

uint8_t sineTable[kSineLen];
bool droneActive = false;
bool micActive = false;

// Perceptual-ish curve: level 0 is silent, level max is full scale.
uint8_t levelToVolume(int level) {
    if (level <= 0) return 0;
    float x = (float)level / settings::kMaxVolume;
    return (uint8_t)constrain((int)lroundf(255.0f * powf(x, 1.5f)), 1, 255);
}

}  // namespace

namespace audio {

void begin() {
    for (size_t i = 0; i < kSineLen; ++i) {
        sineTable[i] = (uint8_t)lroundf(128.0f + 127.0f * sinf(2.0f * (float)M_PI * i / kSineLen));
    }
    setVolumeLevel(settings::get().volume);
}

void setVolumeLevel(int level) { M5Cardputer.Speaker.setVolume(levelToVolume(level)); }

void click(float hz, uint32_t ms) {
    if (micActive) return;
    M5Cardputer.Speaker.tone(hz, ms, kClickChannel, true);
}

void droneStart(float hz) {
    if (micActive) return;
    droneActive = M5Cardputer.Speaker.tone(hz, UINT32_MAX, kDroneChannel, true, sineTable, kSineLen);
    Serial.printf("[audio] drone %.2f Hz: %s (speaker running=%d, volume=%d)\n", hz, droneActive ? "queued" : "FAILED",
                  (int)M5Cardputer.Speaker.isRunning(), (int)M5Cardputer.Speaker.getVolume());
}

void droneStop() {
    if (droneActive) M5Cardputer.Speaker.stop(kDroneChannel);
    droneActive = false;
}

bool droneOn() { return droneActive; }

void stopAll() {
    droneActive = false;
    M5Cardputer.Speaker.stop();
}

bool micStart() {
    if (micActive) return true;
    stopAll();
    M5Cardputer.Speaker.end();
    micActive = M5Cardputer.Mic.begin();
    Serial.printf("[audio] mic begin: %s (enabled=%d, running=%d, rate=%u)\n", micActive ? "ok" : "FAILED",
                  (int)M5Cardputer.Mic.isEnabled(), (int)M5Cardputer.Mic.isRunning(),
                  (unsigned)M5Cardputer.Mic.config().sample_rate);
    if (!micActive) M5Cardputer.Speaker.begin();
    return micActive;
}

void micStop() {
    if (!micActive) return;
    M5Cardputer.Mic.end();
    micActive = false;
    delay(30);  // let the codec settle before the speaker path is brought back up
    const bool ok = M5Cardputer.Speaker.begin();
    Serial.printf("[audio] mic stopped, speaker begin: %s\n", ok ? "ok" : "FAILED");
}

bool micOn() { return micActive; }

}  // namespace audio
