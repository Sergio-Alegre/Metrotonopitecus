// Keyboard abstraction: turns the raw Cardputer key state into discrete events
// with auto-repeat for navigation/volume keys.
//
// The arrow cursors of the Cardputer are the keys ; , . / (printed arrows).
// They are accepted with or without Fn/Shift so they work in a single press.
#pragma once
#include <Arduino.h>

enum class Key : uint8_t { Char, Up, Down, Left, Right, Enter, Del, Space };

struct KeyEvent {
    Key key;
    char ch;  // lowercase character, only meaningful for Key::Char
};

namespace input {

// Call once per loop, right after M5Cardputer.update().
void poll();

// Pops the next pending event. Returns false when the queue is empty.
bool next(KeyEvent& out);

}  // namespace input
