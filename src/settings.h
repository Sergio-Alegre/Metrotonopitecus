// Persistent settings (NVS). Writes are deferred until the values stop
// changing so that holding a key does not wear the flash.
#pragma once
#include <Arduino.h>

struct Settings {
    int volume = 6;          // global volume level, 0..kMaxVolume
    int bpm = 100;           // metronome tempo
    int subdivision = 1;     // metronome clicks per beat (1..4)
    int targetNote = 69;     // tuner target as MIDI note (A4 = 69), C3..B5
    float refA = 442.0f;     // tuner reference for A4, in Hz
};

namespace settings {

constexpr int kMaxVolume = 10;
constexpr int kMinBpm = 20;
constexpr int kMaxBpm = 300;
constexpr int kMinNote = 48;  // C3
constexpr int kMaxNote = 83;  // B5
constexpr float kMinRefA = 400.0f;
constexpr float kMaxRefA = 480.0f;

Settings& get();

void load();

// Marks the settings as changed; they are written after a short idle time.
void touch();

// Call from the main loop to flush pending changes.
void tick();

}  // namespace settings
