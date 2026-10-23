// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// Chunk ring written directly by M5Unified's Mic_Class capture task.
// Chunk n always lives in ring slot n % kRingChunks. The buffer-release callback
// runs in the capture task when chunk n is full: it counts it and queues chunk n + 2,
// so two chunks are always in flight. Chunks n-1 and n (the newest complete pair) are
// safe to read until chunk n + 3 completes, i.e. for two hops (64 ms).
#include "tonemap_audio.h"

#include <M5Cardputer.h>

#include <atomic>
#include <string.h>

#include "esp_timer.h"

#include "audio.h"

namespace tonemap_audio {

namespace {

int16_t ring[kRingChunks][kHop];

// ---- shared with the capture task (callback) ----
std::atomic<uint32_t> done{0};       // chunks completed in this epoch (callback only increments)
std::atomic<uint32_t> nextQueue{0};  // next chunk index to queue in this epoch
std::atomic<bool> run{false};        // callback / service may queue chunks
std::atomic<bool> qBusy{false};      // try-lock around queueing
std::atomic<uint32_t> totalChunks{0};
std::atomic<uint32_t> recFailCnt{0};
std::atomic<uint32_t> badCbCnt{0};

// Seqlock for the (chunk count, completion time) pair read by chunkClock().
std::atomic<uint32_t> clkSeq{0};
std::atomic<uint32_t> clkN{0};
std::atomic<uint32_t> clkUs{0};

// ---- loop task only ----
std::atomic<uint32_t> epochNo{0};
bool wantRun = false;
uint32_t starvedCnt = 0, tornCnt = 0, skippedCnt = 0;
bool haveTaken = false;
uint32_t takenSeq = 0, takenEpoch = 0;
bool idleSeen = false;
uint32_t idleSinceMs = 0;

constexpr uint32_t kStarveMs = 10;  // how long the mic may sit idle before the capture restarts

bool needQueue() { return run.load() && (nextQueue.load() - done.load()) < 2; }

// Keeps two chunks in flight. Callable from the capture callback and from the loop task; the
// try-lock keeps chunks in order. The loop re-checks after unlocking so that a completion that
// found the lock taken is never lost.
void topUp() {
    for (;;) {
        if (qBusy.exchange(true, std::memory_order_acquire)) return;
        bool failed = false;
        while (needQueue()) {
            const uint32_t n = nextQueue.load();
            if (!M5Cardputer.Mic.record(ring[n % kRingChunks], kHop, kSampleRate)) {
                recFailCnt.fetch_add(1);
                failed = true;
                break;
            }
            nextQueue.store(n + 1);
        }
        qBusy.store(false, std::memory_order_release);
        if (failed || !needQueue()) return;
    }
}

// Capture task context: short, no printing, no blocking.
void onRelease(void*, void* data, size_t len) {
    const uint32_t d = done.load(std::memory_order_relaxed);
    if (len != kHop || data != (void*)ring[d % kRingChunks]) {
        badCbCnt.fetch_add(1);
        return;
    }
    const uint32_t us = (uint32_t)esp_timer_get_time();
    const uint32_t total = totalChunks.fetch_add(1) + 1;
    const uint32_t s = clkSeq.load(std::memory_order_relaxed);
    clkSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    clkN.store(total, std::memory_order_relaxed);
    clkUs.store(us, std::memory_order_relaxed);
    clkSeq.store(s + 2, std::memory_order_release);

    done.fetch_add(1, std::memory_order_release);
    topUp();
}

void waitQueueIdle() {
    for (int i = 0; i < 500 && qBusy.load(); ++i) delayMicroseconds(10);
}

// Only when no chunk is in flight (or the mic is known to be idle). New epoch, warm-up again.
void beginEpoch() {
    waitQueueIdle();
    epochNo.fetch_add(1);
    done.store(0);
    nextQueue.store(0);
    haveTaken = false;
    idleSeen = false;
    run.store(true);
    topUp();
}

}  // namespace

void attach() {
    run.store(false);
    wantRun = false;
    waitQueueIdle();
    done.store(0);
    nextQueue.store(0);
    haveTaken = false;
    idleSeen = false;
    M5Cardputer.Mic.setSampleRate(kSampleRate);
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, onRelease);
}

void detach() {
    run.store(false);
    wantRun = false;
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);
    M5Cardputer.Mic.setSampleRate(16000);
}

bool start() {
    if (!audio::micOn()) return false;
    wantRun = true;
    if (!run.load() && done.load() == nextQueue.load()) beginEpoch();  // else service() retries
    return true;
}

void stop() {
    wantRun = false;
    run.store(false);
}

bool running() { return run.load(); }

void service() {
    if (!audio::micOn()) return;
    if (!run.load()) {
        // Deferred (re)start: only when every queued chunk has been delivered.
        if (wantRun && done.load() == nextQueue.load()) beginEpoch();
        return;
    }
    topUp();  // retries a refused record() and covers the unlikely lock miss

    // Starvation: capture running but the mic has nothing queued (lost callback, refused record()).
    if (M5Cardputer.Mic.isRecording() == 0) {
        const uint32_t now = millis();
        if (!idleSeen) {
            idleSeen = true;
            idleSinceMs = now;
        } else if ((uint32_t)(now - idleSinceMs) >= kStarveMs) {
            ++starvedCnt;
            beginEpoch();
        }
    } else {
        idleSeen = false;
    }
}

bool takeWindow(int16_t* dst, WindowInfo& info) {
    if (!run.load()) return false;
    const uint32_t d = done.load(std::memory_order_acquire);
    if (d < 3) return false;  // chunk 0 holds audio from before the (re)start
    const uint32_t ep = epochNo.load();
    const bool sameEpoch = haveTaken && takenEpoch == ep;
    if (sameEpoch && d == takenSeq) return false;  // nothing new

    memcpy(dst, ring[(d - 2) % kRingChunks], kHop * sizeof(int16_t));
    memcpy(dst + kHop, ring[(d - 1) % kRingChunks], kHop * sizeof(int16_t));
    std::atomic_thread_fence(std::memory_order_acquire);
    // Chunk d + 2 would overwrite the first chunk once chunk d + 1 completes (done >= d + 2).
    if (done.load(std::memory_order_acquire) - d > 1) {
        ++tornCnt;
        return false;
    }

    info.seq = d;
    info.epoch = ep;
    info.dtMs = 0;
    if (sameEpoch) {
        skippedCnt += d - takenSeq - 1;
        info.dtMs = (d - takenSeq) * kHopMs;
    }
    haveTaken = true;
    takenSeq = d;
    takenEpoch = ep;
    return true;
}

Stats stats() {
    Stats s;
    s.chunks = totalChunks.load();
    s.starved = starvedCnt;
    s.torn = tornCnt;
    s.skipped = skippedCnt;
    s.recFail = recFailCnt.load();
    s.badCb = badCbCnt.load();
    return s;
}

void chunkClock(uint32_t& n, uint32_t& us) {
    for (int tries = 0; tries < 100; ++tries) {
        const uint32_t s1 = clkSeq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        n = clkN.load(std::memory_order_relaxed);
        us = clkUs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (clkSeq.load(std::memory_order_relaxed) == s1) return;
    }
}

}  // namespace tonemap_audio
