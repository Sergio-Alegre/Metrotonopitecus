// Battery state for the icon in the bottom bar (drawn by main.cpp).
//
// The Cardputer ADV has no power-management chip: M5Unified reads the battery through the ADC
// (GPIO10, divider x2) and maps 3.3 V..4.1 V linearly to 0..100 %. The reading is noisy and sags under
// load, so it is sampled every few seconds, smoothed and shown with five segments only.
#pragma once
#include <Arduino.h>

namespace battery {

constexpr int kUnavailable = -1;  // segments(): no plausible reading
constexpr int kMaxSegments = 5;

// First reading (call once after M5Cardputer.begin()). The icon is valid from here on.
void begin();

// Call every loop iteration; reads at most once every few seconds. Returns true when what the icon
// shows (segment count / unavailable / charging) changed and the bar must be redrawn.
bool tick(uint32_t nowMs);

// Segments to draw: kUnavailable, or 0..kMaxSegments.
int segments();

// True only if the board reports charging (the ADV does not: the state is "unknown", so false).
bool charging();

// Pure helper: level 0..100 -> 0..5 segments, ceil(level / 20), at least 1 while level > 0.
int segmentsForLevel(int level);

}  // namespace battery
