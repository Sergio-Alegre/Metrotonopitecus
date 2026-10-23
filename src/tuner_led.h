// Shared LED + screen-brightness helpers of the two tuner views (classic needle and ToneMap).
//
// The Cardputer ADV RGB LED (SK6812, GPIO21, driven with neopixelWrite) is powered from the same pin
// as the screen backlight (GPIO38), so it only works reliably with the screen at brightness 255 and a
// command sent while the backlight PWM is low is lost. Hence: raise the brightness while the LED
// feature is active, restore it on every exit path, and repeat every command on later loop iterations
// (service()).
//
// The state lives here, in one place, so the two views can never both own the brightness: a view
// switch goes through leave() of one view (offBlocking + brightnessRestore) and then enter() of the
// other (brightnessRaise reads the restored user value). The metronome keeps its own copy of this
// logic (mode_metronome.cpp) and is deliberately not part of this module.
#pragma once
#include <Arduino.h>

namespace tuner_led {

// Forgets a previous run (LED considered off, no pending repeats). Does NOT touch the brightness.
void reset();

// Saves the user's brightness and sets 255 (no-op when already raised or already at 255).
void brightnessRaise();
// Puts the saved user brightness back (no-op when it was not raised).
void brightnessRestore();

// Sets the LED now and schedules the repeats that make up for a command lost to the backlight PWM.
// Does not change lit(): the caller owns that flag.
void show(uint8_t r, uint8_t g, uint8_t b);

// Once per loop iteration while the LED feature is active: sends the pending repeats.
void service();

// Blocking off (3 commands, 6 ms apart), used before the brightness is restored so the commands go
// out while the backlight pin is still held high. Only does something if the LED may be on.
void offBlocking();

// "The LED may be showing something": maintained by the caller, read by offBlocking().
bool lit();
void setLit(bool on);

// Last colour sent (the ToneMap view compares against it to decide when a new colour is worth sending).
uint8_t red();
uint8_t green();
uint8_t blue();

}  // namespace tuner_led
