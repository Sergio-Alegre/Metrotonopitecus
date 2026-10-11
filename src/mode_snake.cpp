// Basic Snake (key G). Silent.
//   arrows   steer the head (the first arrow also starts the game)
//   after dying the score blinks; any key starts a new game
#include <string.h>

#include "modes.h"

namespace {

constexpr int kCell = 5;
constexpr int kCols = ui::kScreenW / kCell;  // 48
constexpr int kRows = 23;
constexpr int kFieldH = kRows * kCell;       // 115
constexpr int kMaxLen = kCols * kRows;

constexpr uint32_t kStartStepMs = 150;
constexpr uint32_t kMinStepMs = 60;
constexpr uint32_t kDeadLockoutMs = 500;  // ignore keys right after dying (held arrows auto-repeat)
constexpr uint32_t kBlinkMs = 400;

struct Pt {
    int8_t x, y;
};

enum class Dir : uint8_t { Up, Down, Left, Right };
enum class State : uint8_t { Ready, Playing, Dead };

Pt delta(Dir d) {
    switch (d) {
        case Dir::Up: return {0, -1};
        case Dir::Down: return {0, 1};
        case Dir::Left: return {-1, 0};
        default: return {1, 0};
    }
}

bool opposite(Dir a, Dir b) {
    return (a == Dir::Up && b == Dir::Down) || (a == Dir::Down && b == Dir::Up) || (a == Dir::Left && b == Dir::Right) ||
           (a == Dir::Right && b == Dir::Left);
}

class SnakeMode : public Mode {
public:
    char hotkey() const override { return 'g'; }
    const char* label() const override { return "Game"; }

    void enter() override { reset(); }

    void onKey(const KeyEvent& e) override {
        const uint32_t now = millis();
        if (state_ == State::Dead) {
            if (now - deadAt_ >= kDeadLockoutMs) reset();
            return;
        }

        Dir d;
        switch (e.key) {
            case Key::Up: d = Dir::Up; break;
            case Key::Down: d = Dir::Down; break;
            case Key::Left: d = Dir::Left; break;
            case Key::Right: d = Dir::Right; break;
            default: return;
        }

        // Compare with the last direction that will be in effect (queued or current).
        const Dir last = queued_ > 0 ? queue_[queued_ - 1] : dir_;
        if (d == last || opposite(d, last)) return;
        if (queued_ < 2) queue_[queued_++] = d;

        if (state_ == State::Ready) {
            state_ = State::Playing;
            lastStep_ = now;
            Serial.println("[snake] start");
        }
        dirty_ = true;
    }

    void update() override {
        const uint32_t now = millis();
        if (state_ == State::Playing) {
            const uint32_t interval = (uint32_t)max((int)kMinStepMs, (int)kStartStepMs - 4 * score_);
            if (now - lastStep_ >= interval) {
                lastStep_ += interval;
                step(now);
                dirty_ = true;
            }
        } else if (state_ == State::Dead) {
            const bool visible = ((now - deadAt_) / kBlinkMs) % 2 == 0;
            if (visible != blinkVisible_) {
                blinkVisible_ = visible;
                dirty_ = true;
            }
        }
    }

    bool draw(M5Canvas& c, bool force) override {
        if (!dirty_ && !force) return false;
        dirty_ = false;

        c.fillSprite(BLACK);
        c.drawRect(0, 0, ui::kScreenW, kFieldH, c.color565(60, 60, 60));

        // Food
        c.fillCircle(food_.x * kCell + 2, food_.y * kCell + 2, 2, RED);

        // Tube body (tail to head), then the head
        const uint16_t tube = c.color565(0, 150, 0);
        const uint16_t shine = c.color565(110, 230, 110);
        for (int i = len_ - 1; i > 0; --i) {
            const float x0 = body_[i].x * kCell + 2.0f, y0 = body_[i].y * kCell + 2.0f;
            const float x1 = body_[i - 1].x * kCell + 2.0f, y1 = body_[i - 1].y * kCell + 2.0f;
            c.drawWedgeLine(x0, y0, x1, y1, 2.4f, 2.4f, tube);
        }
        for (int i = len_ - 1; i > 0; --i) {
            const float x0 = body_[i].x * kCell + 2.0f, y0 = body_[i].y * kCell + 2.0f;
            const float x1 = body_[i - 1].x * kCell + 2.0f, y1 = body_[i - 1].y * kCell + 2.0f;
            c.drawWedgeLine(x0, y0, x1, y1, 0.7f, 0.7f, shine);
        }
        c.fillCircle(body_[0].x * kCell + 2, body_[0].y * kCell + 2, 3, c.color565(0, 230, 0));

        // Score line
        c.setFont(&fonts::Font0);
        c.setTextSize(1);
        c.setTextDatum(top_left);
        c.setTextColor(WHITE);
        c.drawString("Score " + String(score_), 4, kFieldH + 1);
        c.setTextDatum(top_right);
        c.setTextColor(DARKGREY);
        if (state_ == State::Ready) c.drawString("Press an arrow to start", 236, kFieldH + 1);
        if (state_ == State::Dead && millis() - deadAt_ >= kDeadLockoutMs) c.drawString("Press any key", 236, kFieldH + 1);

        // Game over: blinking score
        if (state_ == State::Dead && blinkVisible_) {
            c.fillRect(60, 22, 120, 66, BLACK);
            c.drawRect(60, 22, 120, 66, WHITE);
            c.setTextDatum(middle_center);
            c.setFont(&fonts::Font2);
            c.setTextColor(LIGHTGREY);
            c.drawString("Score", 120, 36);
            c.setFont(&fonts::Font7);
            c.setTextColor(YELLOW);
            c.drawString(String(score_), 120, 64);
        }
        return true;
    }

private:
    void reset() {
        len_ = 3;
        body_[0] = {kCols / 2, kRows / 2};
        body_[1] = {kCols / 2 - 1, kRows / 2};
        body_[2] = {kCols / 2 - 2, kRows / 2};
        dir_ = Dir::Right;
        queued_ = 0;
        score_ = 0;
        state_ = State::Ready;
        blinkVisible_ = true;
        placeFood();
        dirty_ = true;
    }

    bool onBody(int x, int y, int skipTail) const {
        for (int i = 0; i < len_ - skipTail; ++i) {
            if (body_[i].x == x && body_[i].y == y) return true;
        }
        return false;
    }

    void placeFood() {
        for (int tries = 0; tries < 1000; ++tries) {
            const int x = random(kCols), y = random(kRows);
            if (!onBody(x, y, 0)) {
                food_ = {(int8_t)x, (int8_t)y};
                return;
            }
        }
        food_ = {0, 0};  // board practically full
    }

    void step(uint32_t now) {
        if (queued_ > 0) {
            dir_ = queue_[0];
            queue_[0] = queue_[1];
            --queued_;
        }
        const Pt d = delta(dir_);
        const int nx = body_[0].x + d.x, ny = body_[0].y + d.y;
        const bool eats = (nx == food_.x && ny == food_.y);

        // The tail cell is free to enter unless the snake grows this step.
        if (nx < 0 || nx >= kCols || ny < 0 || ny >= kRows || onBody(nx, ny, eats ? 0 : 1)) {
            state_ = State::Dead;
            deadAt_ = now;
            blinkVisible_ = true;
            Serial.printf("[snake] game over, score %d\n", score_);
            return;
        }

        if (eats && len_ < kMaxLen) ++len_;
        memmove(&body_[1], &body_[0], (len_ - 1) * sizeof(Pt));
        body_[0] = {(int8_t)nx, (int8_t)ny};
        if (eats) {
            ++score_;
            placeFood();
        }
    }

    Pt body_[kMaxLen];
    int len_ = 3;
    Pt food_ = {0, 0};
    Dir dir_ = Dir::Right;
    Dir queue_[2] = {Dir::Right, Dir::Right};
    int queued_ = 0;
    int score_ = 0;
    State state_ = State::Ready;
    uint32_t lastStep_ = 0;
    uint32_t deadAt_ = 0;
    bool blinkVisible_ = true;
    bool dirty_ = true;
};

}  // namespace

Mode& snakeMode() {
    static SnakeMode mode;
    return mode;
}
