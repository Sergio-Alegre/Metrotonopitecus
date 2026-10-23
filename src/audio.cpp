#include "audio.h"

#include <M5Cardputer.h>

#include <math.h>

#include "settings.h"

namespace {

constexpr uint8_t kDroneChannel = 0;
constexpr uint8_t kClickChannel = 1;
// One drone cycle, 8-bit unsigned. The speaker reads it at freq * kSineLen Hz,
// which must stay below its 48 kHz output rate (988 Hz * 48 = 47.4 kHz); the
// proven working range of the M5Unified resampler. Linear interpolation of 48
// points per cycle keeps distortion under -50 dB.
constexpr size_t kSineLen = 48;

// Drone timbre: a pure sine sounded thin and nasal on the Cardputer's tiny speaker, which barely reproduces
// the low fundamentals (the 8-bit steps and speaker rattle dominate). The cycle is the fundamental plus a few
// weak harmonics (an organ-flute-like tone): the ear still hears the fundamental pitch (missing-fundamental
// effect) and the note carries on the speaker. Relative amplitudes of harmonics 1..4; tune by ear.
constexpr float kDroneHarmonics[] = {1.0f, 0.5f, 0.25f, 0.12f};

uint8_t sineTable[kSineLen];
bool droneActive = false;
bool micActive = false;

// M5Unified's own ADV microphone callback powers the whole codec down when the
// mic stops and the speaker callback powers it up again, which pops. Replace
// the mic callback with one that does the same register writes on enable but
// leaves the output stage powered on disable. (setCallback is protected, so it
// is reached through a pointer to member named via a derived class.)
struct MicAccess : m5::Mic_Class {
    using m5::Mic_Class::setCallback;
};

uint32_t micCbOn = 0, micCbOff = 0;

bool micCodecCallback(void*, bool enabled) {
    if (!enabled) {
        ++micCbOff;
        return true;
    }
    ++micCbOn;
    static constexpr uint8_t kEnable[][2] = {
        {0x00, 0x80},  // CSM power on
        {0x01, 0xBF},  // clocks: MCLK from BCLK, ADC and DAC clocks on (0xBA would gate the
                       // DAC clock, and switching it back on for the speaker pops)
        {0x02, 0x18},  // MULT_PRE = 3
        {0x0D, 0x01},  // power up analog circuitry
        {0x0E, 0x02},  // enable PGA and ADC modulator
        {0x14, 0x10},  // select Mic1p-Mic1n, minimum PGA gain
        {0x17, 0xBF},  // ADC volume 0 dB
        {0x1C, 0x6A},  // ADC equalizer bypass, cancel DC offset
    };
    for (const auto& r : kEnable) M5.In_I2C.writeRegister8(0x18, r[0], r[1], 100000);
    return true;
}

// The ES8311 codec of the ADV makes a pop whenever M5Unified powers its output
// stage up or down (every mic <-> speaker switch). Muting the DAC around the
// switch and un-muting it with the codec's own volume ramp hides the pop.
constexpr uint8_t kCodecAddr = 0x18;
constexpr uint8_t kRegDacVolume = 0x32;  // 0xBF = 0 dB
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
    float cycle[kSineLen];
    float peak = 0.0f;
    for (size_t i = 0; i < kSineLen; ++i) {
        float v = 0.0f;
        for (size_t h = 0; h < sizeof(kDroneHarmonics) / sizeof(kDroneHarmonics[0]); ++h) {
            v += kDroneHarmonics[h] * sinf(2.0f * (float)M_PI * (float)(h + 1) * i / kSineLen);
        }
        cycle[i] = v;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    for (size_t i = 0; i < kSineLen; ++i) {
        sineTable[i] = (uint8_t)lroundf(128.0f + 127.0f * cycle[i] / peak);  // normalised to full scale
    }
    setVolumeLevel(settings::get().volume);

    auto setMicCallback = &MicAccess::setCallback;
    (M5Cardputer.Mic.*setMicCallback)(nullptr, micCodecCallback);
    // The speaker is started lazily by the first sound: starting it here pops at launch.
}

void setVolumeLevel(int level) { M5Cardputer.Speaker.setVolume(levelToVolume(level)); }

void click(float hz, uint32_t ms, uint8_t gain) {
    if (micActive) return;
    M5Cardputer.Speaker.setChannelVolume(kClickChannel, gain);
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
    if (droneActive) {
        // Cutting a sine mid-wave clicks: fade the codec's DAC volume out first
        // (0xBF = 0 dB, about 0.5 dB per step), then stop and restore the volume.
        auto& i2c = M5.In_I2C;
        for (int v = 0xBF; v >= 0x40; v -= 12) {
            i2c.writeRegister8(kCodecAddr, kRegDacVolume, (uint8_t)v, kI2cFreq);
            delay(3);
        }
        codecMute(true);
        M5Cardputer.Speaker.stop(kDroneChannel);
        delay(5);
        i2c.writeRegister8(kCodecAddr, kRegDacVolume, 0xBF, kI2cFreq);
        unmutePending = true;
        unmuteAt = millis() + kUnmuteDelayMs;
    }
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
    if (!micActive) {
        M5Cardputer.Speaker.begin();
        // micStop() returns early when the mic never started, so schedule the un-mute here: otherwise the
        // codec stays muted (silent metronome / drone) until some later successful mic cycle.
        unmutePending = true;
        unmuteAt = millis() + kUnmuteDelayMs;
    }
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

const char* debugStep(int step) {
    struct RegStep {
        uint8_t reg, val;
        const char* name;
    };
    static const RegStep kRegs[] = {
        {0x00, 0x80, "3: reg 0x00 = 0x80 (CSM power on)"},
        {0x01, 0xBF, "4: reg 0x01 = 0xBF (clocks on)"},
        {0x02, 0x18, "5: reg 0x02 = 0x18 (MULT_PRE)"},
        {0x0D, 0x01, "6: reg 0x0D = 0x01 (analog power up)"},
        {0x0E, 0x02, "7: reg 0x0E = 0x02 (PGA + ADC)"},
        {0x14, 0x10, "8: reg 0x14 = 0x10 (mic select)"},
        {0x17, 0xBF, "9: reg 0x17 = 0xBF (ADC volume)"},
        {0x1C, 0x6A, "10: reg 0x1C = 0x6A (ADC eq)"},
    };
    if (step == 1) {
        codecMute(true);
        return "1: DAC mute (reg 0x31)";
    }
    if (step == 2) {
        M5Cardputer.Speaker.end();
        return "2: Speaker.end()";
    }
    if (step >= 3 && step <= 10) {
        const RegStep& r = kRegs[step - 3];
        M5.In_I2C.writeRegister8(kCodecAddr, r.reg, r.val, kI2cFreq);
        return r.name;
    }
    if (step == 11) {
        micActive = M5Cardputer.Mic.begin();
        return "11: Mic.begin() (I2S clocks start)";
    }
    return "end of sequence";
}

const char* debugCodec() {
    static char buf[48];
    auto& i2c = M5.In_I2C;
    const uint8_t r31 = i2c.readRegister8(kCodecAddr, kRegDacMute, kI2cFreq);
    const uint8_t r37 = i2c.readRegister8(kCodecAddr, kRegDacRamp, kI2cFreq);
    snprintf(buf, sizeof(buf), "r31=%02X r37=%02X p=%d cb=%u/%u", r31, r37, (int)unmutePending, (unsigned)micCbOn,
             (unsigned)micCbOff);
    return buf;
}
#endif

}  // namespace audio
