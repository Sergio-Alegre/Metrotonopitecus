// Placeholder for the fourth (extra) mode, key A. Shows the key reference.
#include "modes.h"

namespace {

class AboutMode : public Mode {
public:
    char hotkey() const override { return 'a'; }
    const char* label() const override { return "About"; }

    bool draw(M5Canvas& c, bool force) override {
        if (!force) return false;
        c.fillSprite(BLACK);
        c.setTextDatum(top_left);

        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextColor(GREEN);
        c.drawString("Metrotonopitecus", 4, 4);

        c.setFont(&fonts::Font0);
        c.setTextColor(WHITE);
        c.drawString("Metronome + tuner for Cardputer ADV", 4, 28);
        c.setTextColor(DARKGREY);
        c.drawString("Build " __DATE__ " " __TIME__, 4, 40);

        c.setTextColor(YELLOW);
        c.drawString("Keys", 4, 58);
        c.setTextColor(WHITE);
        c.drawString("; , . /    arrows (no Fn needed)", 4, 70);
        c.drawString("- _  = +    volume down / up", 4, 80);
        c.drawString("M T G A    switch mode", 4, 90);
        c.drawString("Space      start / stop, drone", 4, 100);
        return true;
    }
};

}  // namespace

Mode& aboutMode() {
    static AboutMode mode;
    return mode;
}
