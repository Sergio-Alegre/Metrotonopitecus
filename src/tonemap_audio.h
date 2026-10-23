// Continuous, gap-free microphone capture for the ToneMap view.
//
// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)
//
// The capture task of M5Unified's Mic_Class writes straight into a ring of four
// 1024-sample chunks. Its buffer-release callback keeps two chunks queued at all
// times, so capture never depends on how long a loop() iteration takes. The
// analysis side asks for the newest complete 2048-sample window (two chunks) and
// gets 31.25 windows per second at 32 kHz. See docs/TONEMAP_DESIGN.md section 2.
//
// Lifecycle (all from the Arduino loop task):
//   attach()          mic must be stopped; selects 32 kHz and installs the callback
//   audio::micStart() the existing speaker/mic arbitration
//   start()           begins queueing chunks
//   service()         every loop iteration: resume / starvation recovery
//   takeWindow()      newest window not yet taken
//   stop()            stops queueing (the chunks in flight drain)
//   audio::micStop(); detach();
#pragma once
#include <Arduino.h>

namespace tonemap_audio {

constexpr uint32_t kSampleRate = 32000;
constexpr size_t kWindow = 2048;
constexpr size_t kHop = 1024;
constexpr size_t kRingChunks = 4;
constexpr uint32_t kHopMs = 32;  // 1024 / 32000 s
// Measured I2S rate / nominal rate (the bench prints a suggestion). Frequencies must be
// computed with effectiveRate(), not kSampleRate.
constexpr float kRateTrim = 1.0f;
constexpr float effectiveRate() { return (float)kSampleRate * kRateTrim; }

// Mic must be ended (not running). Sets 32 kHz and installs the release callback.
// Resets the capture state and the epoch bookkeeping.
void attach();
// After Mic.end() returned: removes the callback and restores the 16 kHz default.
void detach();

// Requires audio::micOn(). Starts queueing chunks; if chunks of a previous run are still
// in flight the start is deferred to service(). Returns false when the mic is not active.
bool start();
// Stops queueing new chunks. The chunks in flight drain on their own (<= 64 ms).
void stop();
bool running();

// Call every loop iteration: resumes a deferred start, tops the queue up and restarts the
// capture if it starved (epoch++).
void service();

struct WindowInfo {
    uint32_t seq;    // index of the window's last chunk + 1; +1 per 32 ms hop
    uint32_t epoch;  // changes at every (re)start; seq restarts from 0
    uint32_t dtMs;   // time since the previous window of the same epoch (multiples of
                     // kHopMs, longer when windows were skipped); 0 for the first window of an epoch
};

// Copies the newest complete window (kWindow samples) not yet taken into dst. Returns false when
// there is nothing new, during warm-up, or when the window was overwritten while copying (torn).
bool takeWindow(int16_t* dst, WindowInfo& info);

struct Stats {
    uint32_t chunks;    // chunks completed since boot
    uint32_t starved;   // capture restarts caused by starvation
    uint32_t torn;      // windows discarded because the ring was overwritten during the copy
    uint32_t skipped;   // complete windows never analysed (taken window was newer)
    uint32_t recFail;   // Mic.record() refusals
    uint32_t badCb;     // callbacks ignored (unexpected buffer or length)
};
Stats stats();

// Completion time of the latest chunk, taken in the capture callback: n = chunks completed
// since boot, us = low 32 bits of esp_timer_get_time(). (n1-n0) * kHop / ((us1-us0) / 1e6)
// is the real I2S sample rate.
void chunkClock(uint32_t& n, uint32_t& us);

}  // namespace tonemap_audio
