// Phase 0: hello world through the final toolchain (PlatformIO -> Launcher).
// Shows a title and the last key pressed, beeps at 440 Hz on key press,
// and mirrors everything on the serial port.
#include <M5Cardputer.h>

static constexpr int kBeepHz = 440;
static constexpr int kBeepMs = 80;

static String lastKey = "-";

static void draw() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextDatum(top_left);
    d.setTextColor(GREEN);
    d.setTextSize(1);
    d.drawString("Hello Metrotonopitecus", 4, 8);
    d.setTextColor(WHITE);
    d.drawString("Last key:", 4, 40);
    d.setTextSize(2);
    d.setTextColor(YELLOW);
    d.drawString(lastKey, 4, 60);
}

static String describe(const Keyboard_Class::KeysState& st) {
    String s;
    for (char c : st.word) s += c;
    if (st.enter) s += "<enter>";
    if (st.del) s += "<del>";
    if (st.tab) s += "<tab>";
    if (st.space && s.isEmpty()) s += "<space>";
    return s;
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Speaker.setVolume(128);

    Serial.println("Hello Metrotonopitecus");
    draw();
}

void loop() {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        String k = describe(M5Cardputer.Keyboard.keysState());
        if (k.isEmpty()) k = "(modifier)";
        lastKey = k;
        M5Cardputer.Speaker.tone(kBeepHz, kBeepMs);
        Serial.printf("key: %s -> beep %d Hz\n", lastKey.c_str(), kBeepHz);
        draw();
    }
}
