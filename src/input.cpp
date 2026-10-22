#include "input.h"

#include <M5Cardputer.h>

namespace {

constexpr size_t kMaxHeld = 8;
constexpr size_t kQueueSize = 16;
constexpr uint32_t kRepeatDelayMs = 450;
constexpr uint32_t kRepeatPeriodMs = 70;

struct Held {
    KeyEvent ev;
    uint32_t since;
    uint32_t last;
};

Held held[kMaxHeld];
size_t heldCount = 0;

KeyEvent queue[kQueueSize];
size_t qHead = 0;
size_t qTail = 0;

void push(const KeyEvent& e) {
    size_t next = (qTail + 1) % kQueueSize;
    if (next == qHead) return;  // queue full: drop
    queue[qTail] = e;
    qTail = next;
}

KeyEvent classify(char c) {
    switch (c) {
        case ';':
        case ':':
            return {Key::Up, 0};
        case '.':
        case '>':
            return {Key::Down, 0};
        case ',':
        case '<':
            return {Key::Left, 0};
        case '/':
        case '?':
            return {Key::Right, 0};
        case ' ':
            return {Key::Space, 0};
        default:
            return {Key::Char, (char)tolower((unsigned char)c)};
    }
}

bool repeatable(const KeyEvent& e) {
    switch (e.key) {
        case Key::Up:
        case Key::Down:
        case Key::Left:
        case Key::Right:
            return true;
        case Key::Char:
            return e.ch == '-' || e.ch == '_' || e.ch == '=' || e.ch == '+';
        default:
            return false;
    }
}

bool same(const KeyEvent& a, const KeyEvent& b) {
    return a.key == b.key && a.ch == b.ch;
}

}  // namespace

namespace input {

void poll() {
    const auto& st = M5Cardputer.Keyboard.keysState();

    KeyEvent cur[kMaxHeld];
    size_t n = 0;
    for (char c : st.word) {
        if (n < kMaxHeld) cur[n++] = classify(c);
    }
    if (st.enter && n < kMaxHeld) cur[n++] = {Key::Enter, 0};
    if (st.del && n < kMaxHeld) cur[n++] = {Key::Del, 0};
    if (st.alt && n < kMaxHeld) cur[n++] = {Key::Alt, 0};  // not repeatable: repeatable() is false for it

    const uint32_t now = millis();
    Held nextHeld[kMaxHeld];
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
        Held h = {cur[i], now, now};
        bool wasHeld = false;
        for (size_t j = 0; j < heldCount; ++j) {
            if (!same(held[j].ev, cur[i])) continue;
            h = held[j];
            wasHeld = true;
            if (repeatable(h.ev) && now - h.since >= kRepeatDelayMs && now - h.last >= kRepeatPeriodMs) {
                push(h.ev);
                h.last = now;
            }
            break;
        }
        if (!wasHeld) push(cur[i]);
        nextHeld[m++] = h;
    }
    for (size_t i = 0; i < m; ++i) held[i] = nextHeld[i];
    heldCount = m;
}

bool next(KeyEvent& out) {
    if (qHead == qTail) return false;
    out = queue[qHead];
    qHead = (qHead + 1) % kQueueSize;
    return true;
}

}  // namespace input
