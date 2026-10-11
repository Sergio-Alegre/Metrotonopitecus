// Sound output and microphone arbitration.
//
// On the Cardputer ADV the speaker and the microphone share the codec clock
// pins, so only one of them can be active at a time: starting the microphone
// shuts the speaker down and the other way round.
#pragma once
#include <Arduino.h>

namespace audio {

void begin();

// Global volume level 0..settings::kMaxVolume (applied to clicks and drone).
void setVolumeLevel(int level);

// Short metronome click (square-ish tone). Ignored while the mic is active.
void click(float hz, uint32_t ms);

// Continuous sine drone. Ignored while the mic is active.
void droneStart(float hz);
void droneStop();
bool droneOn();

void stopAll();

// Microphone: stops the speaker and starts capture. Returns false on failure.
bool micStart();
void micStop();
bool micOn();

}  // namespace audio
