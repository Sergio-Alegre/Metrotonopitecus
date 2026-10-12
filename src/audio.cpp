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

// The ES8311 codec of the ADV makes a pop whenever M5Unified powers its output
// stage up or down (every mic <-> speaker switch). Muting the DAC around the
// switch and un-muting it with the codec's own volume ramp hides the pop.
constexpr uint8_t kCodecAddr = 0x18;
constexpr uint8_t kRegDacMute = 0x31;  // bits 6 and 5 mute the DAC
constexpr uint8_t kRegDacRamp = 0x37;  // bits 7..4: volume ramp rate (0 = no ramp)
constexpr uint32_t kI2cFreq = 100000;
constexpr uint32_t kUnmuteDelayMs = 250;
bool unmutePending = false;
uint32_t unmuteAt = 0;
uint8_t lastMuteReg = 0;

void codecMute(bool mute) {
    auto& i2c = M5.In_I2C;
    uint8_t v = i2c.readRegister8(kCodecAddr, kRegDacMute, kI2cFreq);
    if (mute) {
        v |= 0x60;
    } else {
        i2c.writeRegister8(kCodecAddr, kRegDacRamp, 0x48, kI2cFreq);  // soft ramp, ~0.25 dB per 64 samples
        v &= 0x9F;
    }
    i2c.writeRegister8(kCodecAddr, kRegDacMute, v, kI2cFreq);
    lastMuteReg = i2c.readRegister8(kCodecAddr, kRegDacMute, kI2cFreq);
}

#ifdef TUNER_DEBUG
char dbgStatus[48] = "-";
int16_t dbgSine16[64];
#define DBG_STATUS(...) snprintf(dbgStatus, sizeof(dbgStatus), __VA_ARGS__)
#else
#define DBG_STATUS(...) ((void)0)
#endif

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
    DBG_STATUS("drone %.0fHz tone=%d run=%d vol=%d", hz, (int)droneActive, (int)M5Cardputer.Speaker.isRunning(),
               (int)M5Cardputer.Speaker.getVolume());
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

void update() {
    if (unmutePending && (int32_t)(millis() - unmuteAt) >= 0) {
        unmutePending = false;
        codecMute(false);
        DBG_STATUS("unmute reg31=%02X", lastMuteReg);
    }
}

bool micStart() {
    if (micActive) return true;
    unmutePending = false;
    codecMute(true);
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
    // As in the official M5 example: let the capture in flight finish first.
    for (int i = 0; i < 300 && M5Cardputer.Mic.isRecording(); ++i) delay(1);
    codecMute(true);
    M5Cardputer.Mic.end();
    micActive = false;
    delay(30);  // let the codec settle before the speaker path is brought back up
    const bool ok = M5Cardputer.Speaker.begin();
    unmutePending = true;
    unmuteAt = millis() + kUnmuteDelayMs;
    DBG_STATUS("spk begin=%d en=%d mute=%02X", (int)ok, (int)M5Cardputer.Speaker.isEnabled(), lastMuteReg);
    Serial.printf("[audio] mic stopped, speaker begin: %s\n", ok ? "ok" : "FAILED");
}

bool micOn() { return micActive; }

#ifdef TUNER_DEBUG
void debugTone(char kind, float hz) {
    if (micActive) micStop();
    bool ok = false;
    switch (kind) {
        case 'b':
            ok = M5Cardputer.Speaker.tone(hz, 500, kDroneChannel, true);
            break;
        case 'q':
            ok = M5Cardputer.Speaker.tone(hz, UINT32_MAX, kDroneChannel, true);
            droneActive = ok;
            break;
        case 's':
            for (int i = 0; i < 64; ++i) dbgSine16[i] = (int16_t)lroundf(20000.0f * sinf(2.0f * (float)M_PI * i / 64.0f));
            ok = M5Cardputer.Speaker.playRaw(dbgSine16, 64, (uint32_t)(hz * 64.0f + 0.5f), false, UINT32_MAX, kDroneChannel,
                                             true);
            droneActive = ok;
            break;
    }
    DBG_STATUS("%c %.0fHz ok=%d run=%d play=%d", kind, hz, (int)ok, (int)M5Cardputer.Speaker.isRunning(),
               (int)M5Cardputer.Speaker.isPlaying());
}

const char* debugStatus() { return dbgStatus; }

const char* debugCodec() {
    static char buf[48];
    auto& i2c = M5.In_I2C;
    const uint8_t r31 = i2c.readRegister8(kCodecAddr, kRegDacMute, kI2cFreq);
    const uint8_t r37 = i2c.readRegister8(kCodecAddr, kRegDacRamp, kI2cFreq);
    snprintf(buf, sizeof(buf), "codec r31=%02X r37=%02X pend=%d", r31, r37, (int)unmutePending);
    return buf;
}
#endif

}  // namespace audio
