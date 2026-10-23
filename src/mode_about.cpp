// Placeholder for the fourth (extra) mode, key A. Shows credits and the key reference.
#include "modes.h"

#ifdef TUNER_DEBUG
#include "audio.h"
#endif

namespace {

enum class Arrow { Up, Down, Left, Right };

// Small cursor-key arrow (about 9x9 px) centred on (cx, cy).
void drawArrow(M5Canvas& c, int cx, int cy, Arrow a, uint16_t col) {
    switch (a) {
        case Arrow::Up:
            c.fillTriangle(cx, cy - 5, cx - 4, cy, cx + 4, cy, col);
            c.fillRect(cx - 1, cy, 3, 5, col);
            break;
        case Arrow::Down:
            c.fillTriangle(cx, cy + 5, cx - 4, cy, cx + 4, cy, col);
            c.fillRect(cx - 1, cy - 5, 3, 5, col);
            break;
        case Arrow::Left:
            c.fillTriangle(cx - 5, cy, cx, cy - 4, cx, cy + 4, col);
            c.fillRect(cx, cy - 1, 5, 3, col);
            break;
        case Arrow::Right:
            c.fillTriangle(cx + 5, cy, cx, cy - 4, cx, cy + 4, col);
            c.fillRect(cx - 5, cy - 1, 5, 3, col);
            break;
    }
}

class AboutMode : public Mode {
public:
    char hotkey() const override { return 'a'; }
    const char* label() const override { return "About"; }

#ifdef TUNER_DEBUG
    // Debug build: this screen runs the pop-hunting sequence instead of showing credits.
    void onKey(const KeyEvent& e) override {
        if (e.key == Key::Space) {
            ++step_;
            desc_ = audio::debugStep(step_);
            Serial.printf("[popTest] %s\n", desc_);
            dirty_ = true;
        }
    }

    bool draw(M5Canvas& c, bool force) override {
        if (!force && !dirty_) return false;
        dirty_ = false;
        c.fillSprite(BLACK);
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(ORANGE);
        c.drawString("POP TEST (debug build)", 4, 4);
        c.setTextColor(WHITE);
        c.drawString("Fresh launch, no sound used yet.", 4, 20);
        c.drawString("Press Space once per step and", 4, 30);
        c.drawString("note the step where a pop is heard.", 4, 40);
        c.setTextColor(YELLOW);
        c.drawString(step_ == 0 ? "Next: step 1 of 11" : desc_, 4, 62);
        return true;
    }

private:
    int step_ = 0;
    const char* desc_ = "";
    bool dirty_ = true;
};
#else

    bool draw(M5Canvas& c, bool force) override {
        if (!force) return false;
        c.fillSprite(BLACK);
        c.setTextDatum(top_left);

        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextColor(GREEN);
        c.drawString("Metrotonopitecus", 4, 4);

        c.setFont(&fonts::Font0);
        c.setTextColor(WHITE);
        c.drawString("Developed by Sergio Alegre", 4, 28);
        c.setTextColor(LIGHTGREY);
        c.drawString("www.sergioalegre.com", 4, 39);

        // Credit for the advanced tuner logic (Font0 is 6 px per char: the scheme is dropped so the URL fits 240 px)
        c.setTextColor(LIGHTGREY);
        c.drawString("Logic for the advanced tuner mode", 4, 50);
        c.drawString("created by tonemap.live:", 4, 60);
        c.drawString("github.com/crossrhythm/tonemap.live", 4, 70);

        // Key reference: keys on the left, meaning at x = 62
        constexpr int kDesc = 62;
        c.setTextColor(WHITE);
        const Arrow arrows[] = {Arrow::Up, Arrow::Down, Arrow::Left, Arrow::Right};
        for (int i = 0; i < 4; ++i) drawArrow(c, 9 + 13 * i, 87, arrows[i], WHITE);
        c.drawString("(no Fn needed)", kDesc, 83);

        c.drawString("-   +", 4, 93);
        c.drawString("volume down / up", kDesc, 93);

        c.drawString("M T G A", 4, 103);
        c.drawString("switch mode", kDesc, 103);

        c.drawString("Space", 4, 113);
        c.drawString("start / stop", kDesc, 113);
        return true;
    }
};
#endif

}  // namespace

Mode& aboutMode() {
    static AboutMode mode;
    return mode;
}
