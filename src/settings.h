// Persistent settings (NVS). Writes are deferred until the values stop
// changing so that holding a key does not wear the flash.
#pragma once
#include <Arduino.h>

struct Settings {
    int volume = 6;          // global volume level, 0..kMaxVolume
    int bpm = 100;           // metronome tempo
    int subdivision = 1;     // metronome clicks per beat (1..4)
    int beats = 4;           // metronome beats per bar (1..kMaxBeats)
    int targetNote = 69;     // tuner target as MIDI note (A4 = 69), C3..B5
    float refA = 442.0f;     // tuner reference for A4, in Hz
    // Tuner, ToneMap view (docs/SPEC.md)
    int tunerView = 0;       // 0 classic, 1 ToneMap
    int tmSens = 1;          // 0 relaxed, 1 medium, 2 hard
    int tmHist = 1;          // 0 1 s, 1 3 s, 2 30 s, 3 unlimited
    int tmHold = 2;          // 0 5 s, 1 15 s, 2 30 s, 3 2 min, 4 5 min, 5 no fade
    int tmGate = 2;          // noise gate of the ToneMap view (the classic view is automatic): 0 none, 1 min, 2 low, 3 med, 4 high (NVS key tmGateB)
    int tmTransp = 0;        // transposition in semitones: 0 (C), -2 (Bb), 3 (Eb), 5 (F)
    int tmFlats = 0;         // 0 sharps, 1 flats
    int tmLed = 1;           // in-tune LED on/off
    int tmPalette = 0;       // heat-map colours: 0 safe (blue-white-orange), 1 extreme (green-yellow-red / cyan-blue)
};

namespace settings {

constexpr int kMaxVolume = 10;
constexpr int kMinBpm = 20;
constexpr int kMaxBpm = 300;
constexpr int kMaxBeats = 6;
constexpr int kMinNote = 48;  // C3
constexpr int kMaxNote = 83;  // B5
constexpr float kMinRefA = 400.0f;
constexpr float kMaxRefA = 480.0f;

Settings& get();

// Old noise-gate scale (NVS key "tmGate": 0 none, 1 low, 2 med, 3 high) -> current scale (0 none, 1 min, 2 low, 3 med,
// 4 high): {0,1,2,3} -> {0,2,3,4}. Anything else gives the default (Low, 2).
int migrateGate(int oldValue);

void load();

// Marks the settings as changed; they are written after a short idle time.
void touch();

// Call from the main loop to flush pending changes.
void tick();

}  // namespace settings
