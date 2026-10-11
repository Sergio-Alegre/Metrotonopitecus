#include "audio.h"

#include <M5Cardputer.h>

#include <math.h>

#include "settings.h"

namespace {

constexpr uint8_t kDroneChannel = 0;
constexpr uint8_t kClickChannel = 1;
constexpr size_t kSineLen = 256;

uint8_t sineTable[kSineLen];  // one cycle, 8-bit unsigned
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
    if (!micActive) {
        Serial.println("[audio] microphone failed to start");
        M5Cardputer.Speaker.begin();
    }
    return micActive;
}

void micStop() {
    if (!micActive) return;
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.begin();
    micActive = false;
}

bool micOn() { return micActive; }

}  // namespace audio
