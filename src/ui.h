// Shared UI definitions: screen geometry and the Mode interface.
//
// Layout (240x135): the mode owns the top kContentH rows (drawn through an
// off-screen canvas to avoid flicker); the bottom kBarH rows are the mode bar
// (1 px separator, 1 px gap, one 8 px text line), drawn by main.cpp.
#pragma once
#include <M5Cardputer.h>

#include "input.h"

namespace ui {

constexpr int kScreenW = 240;
constexpr int kScreenH = 135;
constexpr int kBarH = 10;
constexpr int kContentH = kScreenH - kBarH;

}  // namespace ui

class Mode {
public:
    virtual ~Mode() = default;

    virtual char hotkey() const = 0;      // lowercase key that selects the mode
    virtual const char* label() const = 0;  // short English name for the bar

    virtual void enter() {}
    virtual void leave() {}
    virtual void onKey(const KeyEvent&) {}
    virtual void update() {}

    // Draws into the content canvas if needed (or if force is set).
    // Returns true when the canvas changed and must be pushed to the screen.
    virtual bool draw(M5Canvas& canvas, bool force) = 0;
};
