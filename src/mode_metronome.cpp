// Generic metronome (key M).
//   digits        type BPM (Enter confirms, Del erases, 3 digits auto-confirm)
//   Up / Down     BPM +1 / -1 (hold to repeat)
//   Left / Right  subdivision: quarters, eighths, triplets, sixteenths
//   B             beats per bar: 1..6, rotating (6 wraps to 1)
//   Space         start / stop
//
// Click timing runs in its own task so that redrawing the screen in the main
// loop (~15 ms per frame) never shifts a click.
#include <atomic>

#include <esp_timer.h>

#include "audio.h"
#include "modes.h"
#include "settings.h"

namespace {

constexpr uint32_t kEntryTimeoutMs = 3000;
constexpr int kBeatRadius = 8;
constexpr int kBeatSpacing = 26;  // distance between beat centres; 6 beats end at x=152
constexpr int kMargin = 6;        // left margin of the tempo number and of the beats
constexpr int kFlashMinMs = 30;   // LED flash length limits (see afterPush())
constexpr int kFlashMaxMs = 80;
// GPIO38 is both the screen backlight (PWM) and the LED's power switch, so the LED only works
// while the backlight is bright enough (measured on this device: works at 192, not at 160).
// The metronome raises the screen to this level while it is active, with some margin.
constexpr uint8_t kLedMinBrightness = 224;
// Bottom row: beats from the left, "B: beats" hint at the right. The circles' bottom
// (centre + 8 = row 120) leaves 4 free rows above the 125 px content area's end, and the
// hint's text sits on that same bottom line.
constexpr int kBottomY = 112;
constexpr int kHintBottomY = kBottomY + kBeatRadius + 1;  // bottom of the hint's text box
// The tempo block (number, arrows) and the subdivision move together, anchored to the
// vertical centre of the tempo number.
constexpr int kRowY = 62;
constexpr int kArrowsTopY = kRowY + 8;  // top of the Up/Down arrows next to the number
// Vertical centre of the note heads: their bottom (centre + 3) sits on the same line as
// the bottom of the tempo number.
constexpr int kSubBaseY = kRowY + 17;
constexpr int kSubCx = 196;       // horizontal centre of the subdivision figure
// Four sixteenths span x-20..x+21 around the centre; the arrow glyphs are ~10 px wide
// and sit about 7 px outside that.
constexpr int kSubArrowDx = 33;

// The Cardputer ADV's RGB LED (SK6812) is on GPIO21. M5Unified's M5.Led does nothing with this
// Arduino core (it needs ESP-IDF 5), so it is driven with the core's own neopixelWrite().
constexpr int kLedPin = 21;
void ledSet(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(kLedPin, r, g, b); }

std::atomic<bool> gRunning{false};
std::atomic<int> gBpm{100};
std::atomic<int> gSub{1};
std::atomic<int> gBeats{4};
std::atomic<uint32_t> gClickUs{0};  // esp_timer time (low 32 bits, us) of the last click
std::atomic<uint32_t> gSeq{0};  // clicks fired since boot
std::atomic<int> gTickIdx{0};   // index of the last click inside the bar

// Note head: a filled ellipse tilted so its right end rises (like engraved notation),
// drawn pixel by pixel because the canvas has no rotated ellipse. Returns the column of
// its rightmost pixel and the lowest row of that column, where the stem is tangent.
struct HeadEdge {
    int x;
    int y;
};

HeadEdge drawHead(M5Canvas& c, int hx, int hy, uint16_t col) {
    constexpr float kA = 4.8f;                  // semi-axis along the tilt
    constexpr float kB = 3.2f;                  // semi-axis across it
    constexpr float kTilt = 30.0f * PI / 180.0f;
    const float cs = cosf(kTilt);
    const float sn = sinf(kTilt);
    auto inside = [&](int dx, int dy) {
        const float u = dx * cs - dy * sn;  // along the major axis (right end up)
        const float v = dx * sn + dy * cs;
        return (u * u) / (kA * kA) + (v * v) / (kB * kB) <= 1.0f;
    };

    int right = -100;
    for (int dy = -6; dy <= 6; ++dy)
        for (int dx = -7; dx <= 7; ++dx)
            if (inside(dx, dy) && dx > right) right = dx;

    int lowest = -100;
    for (int dy = -6; dy <= 6; ++dy) {
        for (int dx = -7; dx <= 7; ++dx) {
            if (!inside(dx, dy)) continue;
            c.drawPixel(hx + dx, hy + dy, col);
            if (dx == right && dy > lowest) lowest = dy;
        }
    }
    return {hx + right, hy + lowest};
}

// Draws the note figure for a subdivision (1 = quarter, 2 = two beamed
// eighths, 3 = beamed triplet with a "3", 4 = four beamed sixteenths).
// cx: horizontal centre, base: y of the note heads' centre.
// Strokes are thin: 1 px stems (flush with the right edge of the head), 2 px beams.
void drawSubdivision(M5Canvas& c, int sub, int cx, int base, uint16_t col) {
    constexpr int kStem = 24;  // ~3.5 head heights (an octave on a staff); heads are ~7 px tall
    constexpr int kSpacing[] = {0, 0, 14, 14, 11};
    const int n = sub;  // number of notes equals the subdivision
    const int span = kSpacing[sub] * (n - 1);
    const int x0 = cx - span / 2;
    const int top = base - kStem;

    int firstStem = 0;
    int lastStem = 0;
    for (int i = 0; i < n; ++i) {
        const HeadEdge e = drawHead(c, x0 + i * kSpacing[sub], base, col);
        c.fillRect(e.x, top, 1, e.y - top + 1, col);
        if (i == 0) firstStem = e.x;
        lastStem = e.x;
    }
    if (n > 1) {
        const int width = lastStem - firstStem + 1;
        c.fillRect(firstStem, top, width, 2, col);  // main beam
        if (sub == 4) c.fillRect(firstStem, top + 4, width, 2, col);  // second beam
    }
    if (sub == 3) {
        c.setFont(&fonts::Font0);
        c.setTextDatum(bottom_center);
        c.setTextColor(col);
        c.drawString("3", (firstStem + lastStem) / 2, top - 3);
    }
}

void fireClick(int idx, int sub) {
    if (idx == 0) {
        audio::click(1800.0f, 30);  // bar accent
    } else if (idx % sub == 0) {
        audio::click(1200.0f, 25);  // beat
    } else {
        audio::click(800.0f, 15);   // subdivision
    }
}

void engineTask(void*) {
    int idx = 0;
    int64_t next = 0;
    bool wasRunning = false;
    for (;;) {
        if (!gRunning.load()) {
            wasRunning = false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (!wasRunning) {
            wasRunning = true;
            idx = 0;
            next = esp_timer_get_time();
        }
        // Sleep coarsely, then spin for the last stretch to hit the target time.
        while (gRunning.load() && next - esp_timer_get_time() > 1500) vTaskDelay(1);
        if (!gRunning.load()) continue;
        while (esp_timer_get_time() < next) {
        }

        const int sub = gSub.load();
        const int ticksPerBar = gBeats.load() * sub;
        if (idx >= ticksPerBar) idx = 0;  // subdivision changed mid-bar
        fireClick(idx, sub);
        gClickUs.store((uint32_t)esp_timer_get_time());
        gTickIdx.store(idx);
        gSeq.fetch_add(1);
        idx = (idx + 1) % ticksPerBar;

        next += 60000000LL / ((int64_t)gBpm.load() * sub);
    }
}

class MetronomeMode : public Mode {
public:
    char hotkey() const override { return 'm'; }
    const char* label() const override { return "Metro"; }

    void enter() override {
        static bool started = false;
        if (!started) {
            started = true;
            xTaskCreatePinnedToCore(engineTask, "metronome", 4096, nullptr, 3, nullptr, 0);
        }
        gBpm.store(settings::get().bpm);
        gSub.store(settings::get().subdivision);
        gBeats.store(settings::get().beats);
        gRunning.store(false);
        entry_ = "";
        lastSeq_ = gSeq.load();
        dirty_ = true;

        userBrightness_ = M5Cardputer.Display.getBrightness();
        if (userBrightness_ < kLedMinBrightness) M5Cardputer.Display.setBrightness(kLedMinBrightness);
        ledOff();
    }

    void leave() override {
        gRunning.store(false);
        entry_ = "";
        flashPending_ = false;
        ledOff();
        M5Cardputer.Display.setBrightness(userBrightness_);
    }

    void onKey(const KeyEvent& e) override {
        switch (e.key) {
            case Key::Char:
                if (e.ch >= '0' && e.ch <= '9' && entry_.length() < 3) {
                    entry_ += e.ch;
                    entryAt_ = millis();
                    if (entry_.length() == 3) commitEntry();
                } else if (e.ch == 'b') {
                    commitEntry();
                    setBeats(settings::get().beats % settings::kMaxBeats + 1);
                }
#ifdef LED_DEBUG
                else if (e.ch == 'l' || e.ch == 'k') {
                    // Hardware test, steady white for 2 s. L: screen brightness as it is.
                    // K: screen brightness forced to the next level of kTestLevels first
                    // (GPIO38 is shared between the backlight PWM and the LED power switch),
                    // restored afterwards. Press K repeatedly to walk down the levels.
                    if (e.ch == 'k') {
                        constexpr uint8_t kTestLevels[] = {255, 224, 192, 160, 128, 96};
                        testLevel_ = kTestLevels[testIdx_++ % sizeof(kTestLevels)];
                        M5Cardputer.Display.setBrightness(testLevel_);
                        restoreBrightness_ = true;
                    }
                    ledSet(255, 255, 255);
                    ledOn_ = true;
                    ledOffAt_ = millis() + 2000;
                }
#endif
                break;
            case Key::Enter:
                commitEntry();
                break;
            case Key::Del:
                if (entry_.length()) entry_.remove(entry_.length() - 1);
                break;
            case Key::Up:
                commitEntry();
                setBpm(settings::get().bpm + 1);
                break;
            case Key::Down:
                commitEntry();
                setBpm(settings::get().bpm - 1);
                break;
            case Key::Left:
                commitEntry();
                setSub(settings::get().subdivision - 1);
                break;
            case Key::Right:
                commitEntry();
                setSub(settings::get().subdivision + 1);
                break;
            case Key::Space:
                commitEntry();
                gRunning.store(!gRunning.load());
                Serial.printf("[metro] %s at %d BPM\n", gRunning.load() ? "start" : "stop", settings::get().bpm);
                if (gRunning.load()) {
                    lagCount_ = 0;
                } else {
                    flashPending_ = false;
                    ledOff();
                    reportLag();
                }
                break;
        }
        dirty_ = true;
    }

    void update() override {
        if (entry_.length() && millis() - entryAt_ > kEntryTimeoutMs) {
            commitEntry();
            dirty_ = true;
        }
        const uint32_t seq = gSeq.load();
        if (seq != lastSeq_) {
            lastSeq_ = seq;
            dirty_ = true;
            // A beat (not a subdivision click) arms the LED; it fires in afterPush(), i.e.
            // when the frame with the lit circle reaches the screen.
            const int idx = gTickIdx.load();
            if (gRunning.load() && idx % gSub.load() == 0) {
                flashPending_ = true;
                flashAccent_ = (idx == 0);
            }
        }
        if (ledOn_ && (int32_t)(millis() - ledOffAt_) >= 0) ledOff();
    }

    // Flash the RGB LED at the instant the new frame is on screen: red on the bar accent,
    // green on the other beats, same colours as the circles. The flash lasts 1/5 of the beat,
    // limited to 30..80 ms, so it is a constant 80 ms up to 150 BPM and shrinks for fast
    // tempos (66 ms at 180, 54 at 220, 40 at 300).
    void afterPush() override {
        if (!flashPending_) return;
        flashPending_ = false;

        const int beatMs = 60000 / gBpm.load();
        const int flashMs = constrain(beatMs / 5, kFlashMinMs, kFlashMaxMs);
        if (flashAccent_) {
            ledSet(255, 0, 0);
        } else {
            ledSet(0, 255, 0);
        }
        ledOn_ = true;
        ledOffAt_ = millis() + flashMs;

        // Delay between the click and the LED (= the screen's own delay), for the serial report
        const uint32_t lagUs = (uint32_t)esp_timer_get_time() - gClickUs.load();
        lastLagUs_ = lagUs;
        lagSumUs_ += lagUs;
        if (lagCount_ == 0 || lagUs < lagMinUs_) lagMinUs_ = lagUs;
        if (lagUs > lagMaxUs_ || lagCount_ == 0) lagMaxUs_ = lagUs;
        ++lagCount_;
    }

    bool draw(M5Canvas& c, bool force) override {
        if (!dirty_ && !force) return false;
        dirty_ = false;

        const auto& s = settings::get();
        const bool running = gRunning.load();

        c.fillSprite(BLACK);
        c.setTextSize(1);

        // Tempo on the left: the number is centred in a slot as wide as three digits
        // (so 3-digit values touch the left margin), with the Up/Down arrows on its right.
        const bool typing = entry_.length() > 0;
        const String tempo = typing ? entry_ : String(s.bpm);
        c.setFont(&fonts::Font7);
        const int slotW = c.textWidth("000");
        const int numCx = kMargin + slotW / 2;
        c.setTextDatum(middle_center);
        c.setTextColor(typing ? YELLOW : WHITE);
        c.drawString(tempo, numCx, kRowY);
        const int numberW = c.textWidth(tempo);

        // Up/Down arrows (same cue as the tuner's reference A), same colour as the number
        ui::drawUpDown(c, numCx + numberW / 2 + 9, kArrowsTopY, typing ? YELLOW : LIGHTGREY);

        // Beats of the bar
        const int sub = s.subdivision;
        const int beats = s.beats;
        int beat = running ? gTickIdx.load() / sub : -1;
        if (beat >= beats) beat = beats - 1;
        for (int i = 0; i < beats; ++i) {
            // Left-justified: the first beat stays put, more beats extend to the right.
            const int x = kMargin + kBeatRadius + i * kBeatSpacing;
            const uint16_t col = (i == 0) ? RED : GREEN;
            if (i == beat) {
                c.fillCircle(x, kBottomY, kBeatRadius, col);
            } else {
                c.drawCircle(x, kBottomY, kBeatRadius, DARKGREY);
            }
        }

        // Subdivision on the same row as the tempo: note figure between < >. The arrows stay
        // at fixed distances (set by the widest figure, four sixteenths) and turn grey when
        // their key cannot go any further.
        c.setFont(&fonts::Font2);
        c.setTextDatum(middle_center);
        c.setTextColor(sub > 1 ? CYAN : DARKGREY);
        c.drawString("<", kSubCx - kSubArrowDx, kSubBaseY - 8);
        c.setTextColor(sub < 4 ? CYAN : DARKGREY);
        c.drawString(">", kSubCx + kSubArrowDx, kSubBaseY - 8);
        // Four sixteenths look right-heavy between the arrows: nudge them 2 px left.
        drawSubdivision(c, sub, kSubCx - (sub == 4 ? 2 : 0), kSubBaseY, CYAN);

        // Key hint for the beats-per-bar rotation
        c.setFont(&fonts::Font0);
        c.setTextDatum(bottom_right);
        c.setTextColor(LIGHTGREY);
        c.drawString("B: beats", 236, kHintBottomY);

#ifdef LED_DEBUG
        // Diagnostic overlay: is the LED there, did it start, how many flashes, delay after the click
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "bl=%d n=%u lag=%.0fms test=%d (K)", (int)M5Cardputer.Display.getBrightness(),
                 (unsigned)lagCount_, lastLagUs_ / 1000.0f, (int)testLevel_);
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_left);
        c.setTextColor(YELLOW);
        c.drawString(dbg, 4, 90);
#endif
        return true;
    }

private:
    void ledOff() {
        ledOn_ = false;
        ledSet(0, 0, 0);
#ifdef LED_DEBUG
        if (restoreBrightness_) {
            M5Cardputer.Display.setBrightness(max(userBrightness_, kLedMinBrightness));
            restoreBrightness_ = false;
        }
#endif
    }

    void reportLag() {
        if (!lagCount_) return;
        Serial.printf("[led] %u flashes, delay after click: avg %.1f ms, min %.1f, max %.1f\n", (unsigned)lagCount_,
                      lagSumUs_ / 1000.0f / lagCount_, lagMinUs_ / 1000.0f, lagMaxUs_ / 1000.0f);
        lagSumUs_ = 0;
        lagCount_ = 0;
    }

    void commitEntry() {
        if (!entry_.length()) return;
        setBpm(entry_.toInt());
        entry_ = "";
    }

    void setBpm(int bpm) {
        auto& s = settings::get();
        s.bpm = constrain(bpm, settings::kMinBpm, settings::kMaxBpm);
        gBpm.store(s.bpm);
        settings::touch();
    }

    void setBeats(int beats) {
        auto& s = settings::get();
        s.beats = constrain(beats, 1, settings::kMaxBeats);
        gBeats.store(s.beats);
        settings::touch();
        Serial.printf("[metro] beats per bar: %d\n", s.beats);
    }

    void setSub(int sub) {
        auto& s = settings::get();
        s.subdivision = constrain(sub, 1, 4);
        gSub.store(s.subdivision);
        settings::touch();
    }

    String entry_;
    uint32_t entryAt_ = 0;
    uint32_t lastSeq_ = 0;
    bool dirty_ = true;

    bool flashPending_ = false;
    bool flashAccent_ = false;
    bool ledOn_ = false;
    uint32_t ledOffAt_ = 0;
    uint64_t lagSumUs_ = 0;
    uint32_t lagMinUs_ = 0;
    uint32_t lagMaxUs_ = 0;
    uint32_t lagCount_ = 0;
    uint32_t lastLagUs_ = 0;
    uint8_t userBrightness_ = 128;  // screen brightness before the metronome raised it
#ifdef LED_DEBUG
    uint8_t testLevel_ = 0;
    uint8_t testIdx_ = 0;
    bool restoreBrightness_ = false;
#endif
};

}  // namespace

Mode& metronomeMode() {
    static MetronomeMode mode;
    return mode;
}
