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
constexpr int kBeatSpacing = 40;  // distance between beat centres; 6 beats still fit in 240 px
constexpr int kMargin = 6;        // left margin of the tempo number
constexpr int kRowY = 38;         // vertical centre of the tempo / subdivision row
constexpr int kSubCx = 198;       // horizontal centre of the subdivision figure
// Four sixteenths span about x-17..x+18 around the centre; arrows sit 4 px outside that.
constexpr int kSubArrowDx = 27;

std::atomic<bool> gRunning{false};
std::atomic<int> gBpm{100};
std::atomic<int> gSub{1};
std::atomic<int> gBeats{4};
std::atomic<uint32_t> gSeq{0};  // clicks fired since boot
std::atomic<int> gTickIdx{0};   // index of the last click inside the bar

// Draws the note figure for a subdivision (1 = quarter, 2 = two beamed
// eighths, 3 = beamed triplet with a "3", 4 = four beamed sixteenths).
// cx: horizontal centre, base: y of the note heads' centre.
// Strokes are thin: 1 px stems, 2 px beams.
void drawSubdivision(M5Canvas& c, int sub, int cx, int base, uint16_t col) {
    constexpr int kStem = 14;
    constexpr int kSpacing[] = {0, 0, 14, 14, 9};
    const int n = sub;  // number of notes equals the subdivision
    const int span = kSpacing[sub] * (n - 1);
    const int x0 = cx - span / 2;

    for (int i = 0; i < n; ++i) {
        const int hx = x0 + i * kSpacing[sub];
        c.fillEllipse(hx, base, 4, 3, col);
        c.fillRect(hx + 3, base - kStem, 1, kStem, col);  // stem on the right of the head
    }
    if (n > 1) {
        const int left = x0 + 3;
        const int width = span + 1;
        c.fillRect(left, base - kStem, width, 2, col);  // main beam
        if (sub == 4) c.fillRect(left, base - kStem + 4, width, 2, col);  // second beam
    }
    if (sub == 3) {
        c.setFont(&fonts::Font0);
        c.setTextDatum(bottom_center);
        c.setTextColor(col);
        c.drawString("3", cx + 1, base - kStem - 3);
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
    }

    void leave() override {
        gRunning.store(false);
        entry_ = "";
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
        }
    }

    bool draw(M5Canvas& c, bool force) override {
        if (!dirty_ && !force) return false;
        dirty_ = false;

        const auto& s = settings::get();
        const bool running = gRunning.load();

        c.fillSprite(BLACK);
        c.setTextSize(1);

        // Tempo on the left: the number is centred in a slot as wide as three digits
        // (so 3-digit values touch the left margin), "BPM" as a subscript at its bottom right.
        const bool typing = entry_.length() > 0;
        const String tempo = typing ? entry_ : String(s.bpm);
        c.setFont(&fonts::Font7);
        const int slotW = c.textWidth("000");
        const int numCx = kMargin + slotW / 2;
        c.setTextDatum(middle_center);
        c.setTextColor(typing ? YELLOW : WHITE);
        c.drawString(tempo, numCx, kRowY);
        const int numberW = c.textWidth(tempo);

        // Up/Down arrows sit to the left of "BPM" (same cue as the tuner's reference A)
        const uint16_t bpmCol = typing ? YELLOW : LIGHTGREY;
        const int bpmX = numCx + numberW / 2 + 18;
        c.setFont(&fonts::Font2);
        c.setTextDatum(bottom_left);
        c.setTextColor(bpmCol);
        c.drawString("BPM", bpmX, 62);
        ui::drawUpDown(c, bpmX - 9, 62 - 16, bpmCol);

        // Beats of the bar
        const int sub = s.subdivision;
        const int beats = s.beats;
        int beat = running ? gTickIdx.load() / sub : -1;
        if (beat >= beats) beat = beats - 1;
        for (int i = 0; i < beats; ++i) {
            // Centred as a group: one beat sits in the middle, more spread out symmetrically.
            const int x = 120 + (2 * i - (beats - 1)) * (kBeatSpacing / 2);
            const uint16_t col = (i == 0) ? RED : GREEN;
            if (i == beat) {
                c.fillCircle(x, 88, kBeatRadius, col);
            } else {
                c.drawCircle(x, 88, kBeatRadius, DARKGREY);
            }
        }

        // Subdivision on the same row as the tempo: note figure between < >. The arrows stay
        // at fixed distances (set by the widest figure, four sixteenths) and an arrow is
        // hidden when its key cannot go any further.
        c.setFont(&fonts::Font2);
        c.setTextDatum(middle_center);
        c.setTextColor(CYAN);
        if (sub > 1) c.drawString("<", kSubCx - kSubArrowDx, kRowY + 2);
        if (sub < 4) c.drawString(">", kSubCx + kSubArrowDx, kRowY + 2);
        drawSubdivision(c, sub, kSubCx, kRowY + 6, CYAN);

        // Key hint for the beats-per-bar rotation
        c.setFont(&fonts::Font0);
        c.setTextDatum(top_right);
        c.setTextColor(LIGHTGREY);
        c.drawString("B: beats", 236, 115);
        return true;
    }

private:
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
};

}  // namespace

Mode& metronomeMode() {
    static MetronomeMode mode;
    return mode;
}
