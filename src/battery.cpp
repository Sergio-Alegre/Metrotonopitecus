// Battery reading, smoothing and hysteresis for the bar icon. See battery.h.
#include "battery.h"

#include <M5Cardputer.h>

namespace battery {

namespace {

constexpr uint32_t kRefreshMs = 5000;  // one reading every 5 s
constexpr int kBurst = 5;              // ADC reads per reading; the median is kept
constexpr int kHistory = 3;            // readings in the median filter
constexpr int kPersist = 2;            // a new segment count must be seen this many readings in a row
constexpr int kMinPlausibleMv = 2500;  // below this the ADC pin is not reading a LiPo cell (0 = failed read)
constexpr int kMaxPlausibleMv = 4700;

int history[kHistory];     // last readings, level 0..100 or -1 (unavailable)
int historyPos = 0;
int shown = kUnavailable;  // segments currently drawn
int pending = kUnavailable;
int pendingCount = 0;
bool chargingNow = false;
uint32_t lastReadMs = 0;

#if defined(TUNER_DEBUG) || defined(TONEMAP_DEBUG)
int dbgRawLevel = -1;
int dbgMv = 0;
int dbgChg = 0;
#endif

int median3(int a, int b, int c) {
    if (a > b) { const int t = a; a = b; b = t; }
    if (b > c) { const int t = b; b = c; c = t; }
    if (a > b) { const int t = a; a = b; b = t; }
    return b;
}

// One reading: median of a short burst of level reads, or -1 when the voltage is not a plausible cell voltage
// (ADC read failed = 0, floating pin without a battery) or the level API itself reports an error (< 0).
int readLevel() {
    int v[kBurst];
    for (int i = 0; i < kBurst; ++i) v[i] = (int)M5.Power.getBatteryLevel();
    for (int i = 1; i < kBurst; ++i) {  // insertion sort, kBurst is tiny
        const int x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
    const int level = v[kBurst / 2];
    const int mv = (int)M5.Power.getBatteryVoltage();
#if defined(TUNER_DEBUG) || defined(TONEMAP_DEBUG)
    dbgRawLevel = level;
    dbgMv = mv;
#endif
    if (level < 0 || mv < kMinPlausibleMv || mv > kMaxPlausibleMv) return -1;
    return level > 100 ? 100 : level;
}

int smoothedSegments() {
    return segmentsForLevel(median3(history[0], history[1], history[2]));
}

}  // namespace

int segmentsForLevel(int level) {
    if (level < 0) return kUnavailable;
    if (level == 0) return 0;
    const int s = (level + 19) / 20;
    return s > kMaxSegments ? kMaxSegments : s;
}

void begin() {
    const int level = readLevel();
    for (int i = 0; i < kHistory; ++i) history[i] = level;  // the first reading shows at once
    historyPos = 0;
    shown = segmentsForLevel(level);
    pending = shown;
    pendingCount = 0;
    chargingNow = M5.Power.isCharging() == m5::Power_Class::is_charging;
    lastReadMs = millis();
#if defined(TUNER_DEBUG) || defined(TONEMAP_DEBUG)
    Serial.begin(115200);  // the app does not open the USB CDC port by itself
    dbgChg = (int)M5.Power.isCharging();
    Serial.printf("[battery] raw=%d mv=%d charging=%d (0 no, 1 yes, 2 unknown) segs=%d\n", dbgRawLevel, dbgMv, dbgChg,
                  shown);
#endif
}

bool tick(uint32_t nowMs) {
    if ((uint32_t)(nowMs - lastReadMs) < kRefreshMs) return false;
    lastReadMs = nowMs;

    history[historyPos] = readLevel();
    historyPos = (historyPos + 1) % kHistory;
    const int candidate = smoothedSegments();

    bool changed = false;
    if (candidate == shown) {
        pendingCount = 0;
    } else {
        if (candidate == pending) {
            ++pendingCount;
        } else {
            pending = candidate;
            pendingCount = 1;
        }
        if (pendingCount >= kPersist) {
            shown = candidate;
            pendingCount = 0;
            changed = true;
        }
    }

    const bool chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
    if (chg != chargingNow) {
        chargingNow = chg;
        changed = true;
    }

#if defined(TUNER_DEBUG) || defined(TONEMAP_DEBUG)
    dbgChg = (int)M5.Power.isCharging();
    Serial.printf("[battery] raw=%d mv=%d charging=%d (0 no, 1 yes, 2 unknown) hist=%d,%d,%d cand=%d segs=%d\n",
                  dbgRawLevel, dbgMv, dbgChg, history[0], history[1], history[2], candidate, shown);
#endif
    return changed;
}

int segments() { return shown; }

bool charging() { return chargingNow; }

}  // namespace battery
