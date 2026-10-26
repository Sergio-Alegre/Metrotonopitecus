#include "settings.h"

#include <Preferences.h>

namespace {

constexpr uint32_t kSaveIdleMs = 1500;

Settings current;
Preferences prefs;
bool dirty = false;
uint32_t lastTouch = 0;

void clampAll(Settings& s) {
    s.volume = constrain(s.volume, 0, settings::kMaxVolume);
    s.bpm = constrain(s.bpm, settings::kMinBpm, settings::kMaxBpm);
    s.subdivision = constrain(s.subdivision, 1, 4);
    s.beats = constrain(s.beats, 1, settings::kMaxBeats);
    s.targetNote = constrain(s.targetNote, settings::kMinNote, settings::kMaxNote);
    s.refA = constrain(s.refA, settings::kMinRefA, settings::kMaxRefA);
    s.tunerView = constrain(s.tunerView, 0, 1);
    s.tmSens = constrain(s.tmSens, 0, 2);
    s.tmHist = constrain(s.tmHist, 0, 3);
    s.tmHold = constrain(s.tmHold, 0, 5);
    s.tmGate = constrain(s.tmGate, 0, 4);
    if (s.tmTransp != 0 && s.tmTransp != -2 && s.tmTransp != 3 && s.tmTransp != 5) s.tmTransp = 0;
    s.tmFlats = constrain(s.tmFlats, 0, 1);
    s.tmLed = constrain(s.tmLed, 0, 1);
    s.tmPalette = constrain(s.tmPalette, 0, 1);
    s.tmTemper = constrain(s.tmTemper, 0, settings::kTemperCount - 1);
}

}  // namespace

namespace settings {

Settings& get() { return current; }

int migrateGate(int oldValue) {
    switch (oldValue) {
        case 0: return 0;  // none
        case 1: return 2;  // low
        case 2: return 3;  // med
        case 3: return 4;  // high
        default: return 2;
    }
}

void load() {
    if (!prefs.begin("metro", true)) {
        Serial.println("[settings] NVS not available, using defaults");
        return;
    }
    current.volume = prefs.getInt("volume", current.volume);
    current.bpm = prefs.getInt("bpm", current.bpm);
    current.subdivision = prefs.getInt("subdiv", current.subdivision);
    current.beats = prefs.getInt("beats", current.beats);
    current.targetNote = prefs.getInt("note", current.targetNote);
    current.refA = prefs.getFloat("refA", current.refA);
    current.tunerView = prefs.getInt("tunerView", current.tunerView);
    current.tmSens = prefs.getInt("tmSens", current.tmSens);
    current.tmHist = prefs.getInt("tmHist", current.tmHist);
    current.tmHold = prefs.getInt("tmHold", current.tmHold);
    // The gate has 5 levels since the practice-mute change; the key "tmGate" holds the OLD 4-level scale, so the new
    // scale lives in "tmGateB". If it is absent, an old value is migrated (and the next save writes only the new key).
    if (prefs.isKey("tmGateB")) {
        current.tmGate = prefs.getInt("tmGateB", current.tmGate);
    } else if (prefs.isKey("tmGate")) {
        current.tmGate = migrateGate(prefs.getInt("tmGate", 1));
        Serial.printf("[settings] noise gate migrated to the new scale: %d\n", current.tmGate);
    }
    current.tmTransp = prefs.getInt("tmTransp", current.tmTransp);
    current.tmFlats = prefs.getInt("tmFlats", current.tmFlats);
    current.tmLed = prefs.getInt("tmLed", current.tmLed);
    current.tmPalette = prefs.getInt("tmPal", current.tmPalette);
    current.tmTemper = prefs.getInt("tmTemper", current.tmTemper);
    prefs.end();
    clampAll(current);
    Serial.printf("[settings] loaded: vol=%d bpm=%d subdiv=%d beats=%d note=%d refA=%.1f\n", current.volume,
                  current.bpm, current.subdivision, current.beats, current.targetNote, current.refA);
    Serial.printf("[settings] tuner: view=%d sens=%d hist=%d hold=%d gate=%d transp=%d flats=%d led=%d pal=%d temper=%d\n",
                  current.tunerView, current.tmSens, current.tmHist, current.tmHold, current.tmGate, current.tmTransp,
                  current.tmFlats, current.tmLed, current.tmPalette, current.tmTemper);
}

void touch() {
    clampAll(current);
    dirty = true;
    lastTouch = millis();
}

void tick() {
    if (!dirty || millis() - lastTouch < kSaveIdleMs) return;
    dirty = false;
    if (!prefs.begin("metro", false)) {
        Serial.println("[settings] NVS not writable, changes not saved");
        return;
    }
    prefs.putInt("volume", current.volume);
    prefs.putInt("bpm", current.bpm);
    prefs.putInt("subdiv", current.subdivision);
    prefs.putInt("beats", current.beats);
    prefs.putInt("note", current.targetNote);
    prefs.putFloat("refA", current.refA);
    prefs.putInt("tunerView", current.tunerView);
    prefs.putInt("tmSens", current.tmSens);
    prefs.putInt("tmHist", current.tmHist);
    prefs.putInt("tmHold", current.tmHold);
    prefs.putInt("tmGateB", current.tmGate);
    prefs.putInt("tmTransp", current.tmTransp);
    prefs.putInt("tmFlats", current.tmFlats);
    prefs.putInt("tmLed", current.tmLed);
    prefs.putInt("tmPal", current.tmPalette);
    prefs.putInt("tmTemper", current.tmTemper);
    prefs.end();
    Serial.println("[settings] saved");
}

}  // namespace settings
