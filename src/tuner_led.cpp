// Shared LED + brightness helpers of the tuner views. See tuner_led.h.
// Moved verbatim from tonemap_view.cpp (same constants, same behaviour).
#include "tuner_led.h"

#include <M5Cardputer.h>

namespace tuner_led {

namespace {

constexpr int kLedPin = 21;
constexpr uint8_t kLedMinBrightness = 255;
constexpr int kLedResends = 2;
constexpr uint32_t kLedResendMs = 6;
constexpr int kLedOffSends = 3;

bool brightRaised = false;
uint8_t userBrightness = 128;
bool ledLit = false;
int ledResends = 0;
uint32_t ledResendAt = 0;
uint8_t ledR = 0, ledG = 0, ledB = 0;

// The LED (SK6812) is on GPIO21 and M5.Led does nothing with this Arduino core.
void ledSend(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(kLedPin, r, g, b); }

}  // namespace

void reset() {
    ledLit = false;
    ledResends = 0;
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

void show(uint8_t r, uint8_t g, uint8_t b) {
    ledR = r;
    ledG = g;
    ledB = b;
    ledSend(r, g, b);
    ledResends = kLedResends;
    ledResendAt = millis() + kLedResendMs;
}

void service() {
    if (ledResends > 0 && (int32_t)(millis() - ledResendAt) >= 0) {
        ledSend(ledR, ledG, ledB);
        --ledResends;
        ledResendAt += kLedResendMs;
    }
}

void offBlocking() {
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

bool lit() { return ledLit; }
void setLit(bool on) { ledLit = on; }
uint8_t red() { return ledR; }
uint8_t green() { return ledG; }
uint8_t blue() { return ledB; }

}  // namespace tuner_led
