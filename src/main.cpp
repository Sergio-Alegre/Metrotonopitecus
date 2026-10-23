// Metrotonopitecus: metronome, tuner and a small game for the Cardputer ADV.
// The whole interface is silent except for musical sound (metronome click,
// tuner drone). All on-screen and serial text is English.
#include <M5Cardputer.h>

#include "audio.h"
#include "battery.h"
#include "input.h"
#include "modes.h"
#include "settings.h"
#include "ui.h"

namespace {

constexpr int kCharW = 6;  // Font0 advance at size 1
constexpr int kBarGap = 8;

// Battery icon (bottom-right of the bar): 12 x 7 body + 1 px nub = 13 px wide, 5 segments of 2 x 5 px inside the
// 1 px outline. Rows y + 2 .. y + 8 are the rows of the Font0 capitals of the bar text.
constexpr int kBatW = 13;
constexpr int kBatBodyW = 12;
constexpr int kBatH = 7;
constexpr int kBatRightMargin = 1;   // free columns right of the nub
constexpr int kBatGap = 3;           // between the Vol text and the icon
constexpr uint16_t kVolColor = WHITE;

M5Canvas canvas(&M5Cardputer.Display);

Mode* modes[] = {
    &metronomeMode(),
    &tunerMode(),
    &snakeMode(),
    &aboutMode(),
};
constexpr size_t kModeCount = sizeof(modes) / sizeof(modes[0]);

Mode* current = nullptr;
bool forceRedraw = true;

// Five-segment battery. Normal = the colour of the Vol text, exactly 2 segments = yellow, 1 or 0 = red (outline and
// segments); no plausible reading = grey outline without segments. If the board ever reports charging the outline
// (and the nub) is white.
void drawBattery(M5GFX& d, int x0, int y0) {
    const int segs = battery::segments();
    uint16_t col = kVolColor;
    if (segs < 0) {
        col = DARKGREY;
    } else if (segs <= 1) {
        col = RED;
    } else if (segs == 2) {
        col = YELLOW;
    }
    const uint16_t outline = battery::charging() ? (uint16_t)WHITE : col;
    d.drawRect(x0, y0, kBatBodyW, kBatH, outline);
    d.fillRect(x0 + kBatBodyW, y0 + 2, kBatW - kBatBodyW, 3, outline);
    for (int i = 0; i < segs; ++i) d.fillRect(x0 + 1 + i * 2, y0 + 1, 2, kBatH - 2, col);
}

void drawBar() {
    auto& d = M5Cardputer.Display;
    const int y = ui::kContentH;
    d.fillRect(0, y, ui::kScreenW, ui::kBarH, BLACK);
    d.drawFastHLine(0, y, ui::kScreenW, DARKGREY);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextDatum(top_left);

    int x = 2;
    for (Mode* m : modes) {
        String text = String((char)toupper(m->hotkey())) + ":" + m->label();
        const int w = text.length() * kCharW;
        if (m == current) {
            d.fillRect(x - 2, y + 1, w + 4, ui::kBarH - 1, WHITE);
            d.setTextColor(BLACK, WHITE);
        } else {
            d.setTextColor(LIGHTGREY, BLACK);
        }
        d.drawString(text, x, y + 2);
        x += w + kBarGap;
    }

    // Right end: volume text, then the battery icon pegged at the right edge. The mode labels keep their
    // positions; the volume text moves left by the icon width. labelsEnd = first column free after the
    // (highlighted) last label; "Vol 10" would overlap it, so it loses its space ("Vol10") only when needed.
    const int labelsEnd = x - kBarGap + 2;
    const int batX = ui::kScreenW - kBatRightMargin - kBatW;
    const int volRight = batX - kBatGap;
    String vol = "Vol " + String(settings::get().volume);
    if (volRight - (int)vol.length() * kCharW < labelsEnd + 3) vol = "Vol" + String(settings::get().volume);
    d.setTextColor(kVolColor, BLACK);
    d.drawString(vol, volRight - (int)vol.length() * kCharW, y + 2);
    drawBattery(d, batX, y + 2);
}

void switchTo(Mode* next) {
    if (next == current) return;
    if (current) current->leave();
    current = next;
    current->enter();
    forceRedraw = true;
    drawBar();
    Serial.printf("[mode] %s\n", current->label());
}

void changeVolume(int delta) {
    auto& s = settings::get();
    int v = constrain(s.volume + delta, 0, settings::kMaxVolume);
    if (v == s.volume) return;
    s.volume = v;
    audio::setVolumeLevel(v);
    settings::touch();
    drawBar();
    Serial.printf("[volume] %d/%d\n", v, settings::kMaxVolume);
}

void handleKey(const KeyEvent& e) {
    if (e.key == Key::Alt && !current->acceptsAlt()) return;  // only the ToneMap view uses Alt
    if (e.key == Key::Char) {
        if (e.ch == '-' || e.ch == '_') return changeVolume(-1);
        if (e.ch == '=' || e.ch == '+') return changeVolume(+1);
        for (Mode* m : modes) {
            if (m->hotkey() != e.ch) continue;
            if (m == current) return current->onReselect();
            return switchTo(m);
        }
    }
    current->onKey(e);
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);

    canvas.setColorDepth(16);
    canvas.createSprite(ui::kScreenW, ui::kContentH);

    settings::load();
    audio::begin();
    battery::begin();

    Serial.println("Metrotonopitecus ready");
    switchTo(modes[0]);
}

void loop() {
    M5Cardputer.update();
    input::poll();

    KeyEvent e;
    while (input::next(e)) handleKey(e);

    settings::tick();
    audio::update();
    if (battery::tick(millis())) drawBar();
    current->update();
    if (current->draw(canvas, forceRedraw)) {
        canvas.pushSprite(0, 0);
        M5Cardputer.Display.waitDisplay();  // frame fully transferred
        current->afterPush();
    }
    forceRedraw = false;

    delay(4);
}
