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
}

}  // namespace

namespace settings {

Settings& get() { return current; }

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
    prefs.end();
    clampAll(current);
    Serial.printf("[settings] loaded: vol=%d bpm=%d subdiv=%d beats=%d note=%d refA=%.1f\n", current.volume,
                  current.bpm, current.subdivision, current.beats, current.targetNote, current.refA);
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
    prefs.end();
    Serial.println("[settings] saved");
}

}  // namespace settings
