// Metrotonopitecus: metronome, tuner and a small game for the Cardputer ADV.
// The whole interface is silent except for musical sound (metronome click,
// tuner drone). All on-screen and serial text is English.
#include <M5Cardputer.h>

#include "audio.h"
#include "input.h"
#include "modes.h"
#include "settings.h"
#include "ui.h"

namespace {

constexpr int kCharW = 6;  // Font0 advance at size 1
constexpr int kBarGap = 8;

M5Canvas canvas(&M5Cardputer.Display);

Mode* modes[] = {
    &aboutMode(),
};
constexpr size_t kModeCount = sizeof(modes) / sizeof(modes[0]);

Mode* current = nullptr;
bool forceRedraw = true;

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

    String vol = "Vol " + String(settings::get().volume);
    d.setTextColor(CYAN, BLACK);
    d.drawString(vol, ui::kScreenW - (int)vol.length() * kCharW - 2, y + 2);
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
    if (e.key == Key::Char) {
        if (e.ch == '-' || e.ch == '_') return changeVolume(-1);
        if (e.ch == '=' || e.ch == '+') return changeVolume(+1);
        for (Mode* m : modes) {
            if (m->hotkey() == e.ch) return switchTo(m);
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

    Serial.println("Metrotonopitecus ready");
    switchTo(modes[0]);
}

void loop() {
    M5Cardputer.update();
    input::poll();

    KeyEvent e;
    while (input::next(e)) handleKey(e);

    settings::tick();
    current->update();
    if (current->draw(canvas, forceRedraw)) canvas.pushSprite(0, 0);
    forceRedraw = false;

    delay(4);
}
