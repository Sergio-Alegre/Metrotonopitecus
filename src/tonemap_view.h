// ToneMap view of the tuner: heat map of intonation (12 notes x 6 octaves), info strip,
// keys, LED and the analysis schedule. Used by mode_tuner.cpp, which delegates to these
// functions while the ToneMap view is the active tuner view.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// See docs/TONEMAP_DESIGN.md sections 4.4, 6, 7 and 8.2.
#pragma once
#include <M5Cardputer.h>

#include "input.h"

namespace tonemap {

// Starts the microphone capture (32 kHz), raises the screen brightness when the LED is enabled and
// syncs the heat map with the saved settings. The map itself survives enter()/leave().
void enter();

// Stops the capture and the microphone, switches the LED off and restores the screen
// brightness. Idempotent. Every exit path (T, M/G/A mode switches, pause) goes through it.
void leave();

// Keys of the ToneMap view. Global keys (M G A T - =) never reach it.
void onKey(const KeyEvent& e);

// Once per loop(): capture service, analysis of the newest window, LED, timers.
void update();

// Draws the whole frame into the content canvas when something changed (at most ~20 fps).
// Returns true when the canvas must be pushed.
bool draw(M5Canvas& canvas, bool force);

// Right after the canvas was pushed (debug build: measures the push time).
void afterPush();

}  // namespace tonemap
