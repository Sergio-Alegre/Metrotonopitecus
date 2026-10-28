# ToneMap on the ESP32-S3 — design (phase 2)

Output of the read-only `Plan` agent over `TONEMAP_BRIEF.md` §2–4 and §3.6, `CLAUDE.md`, `src/*` and M5Unified 0.2.25. CPU times are **estimates**, measured in phase 3. §11 holds the orchestrator's decisions and §12 the measured values (filled in phases 3 and 4). Later sections (§13–§16) record changes after device tests.

## 0. Headlines and warnings

1. **Brief §4.1 ("`record()` fails if the queue is full") is wrong in 0.2.25.** From a normal task `Mic_Class::_rec_raw()` **blocks** (retries a 1-tick `xSemaphoreTake`) until a slot frees; it returns `false` immediately only when called from the release callback or with wrong arguments/start. Never call `record()` from `loop()` unless `isRecording() < 2`. The classic tuner is safe because it does `if (isRecording()) return;`.
2. **A public release callback exists**: `Mic_Class::setBufferReleaseCallback(args, func(args, data, len))`, invoked from the capture task when a request finishes; `record()` may be called inside it. Allows gap-free capture independent of `loop()` (an iteration with analysis + draw + push + `delay(4)` can take ~30 ms, almost one 32 ms hop: polling would starve the queue). **The callback is recommended**; polling stays as a documented alternative (§2).
3. **τmax "~62 Hz" (brief §4.2) is too high.** Written C2 with Bb is concert Bb1 = 58.3 Hz (A=442); with A=400, 53 Hz. Use **maxLag = 720 at 32 kHz (44.4 Hz)**. A lobe still open at the end of the sweep needs the end rule (§3).
4. **The store is indexed by concert MIDI and errors are stored relative to A=440.** Same observable behavior as ToneMap (the cell shifts with transposition, A re-scores), with exact mechanisms and no drift (§5).
5. **Intentional differences from pitchy/ToneMap:** the detector subtracts the mean (DC offset of the int16 mic; pitchy does not); the NSDF sweep stops at maxLag, not N−1; a lobe open at the end is accepted if its peak is already behind; Unlimited is truly unlimited (ToneMap prunes at ~33 s / 10 min); Relaxed's EMA fade-in uses the tick's dt as Δ, not the render interval.
6. **Pause:** "the map freezes" is implemented by stopping the map clock during pause (fades freeze too).
7. **LED resends at 6 and 12 ms cannot be met exactly** (iteration up to ~30 ms): they fire in the next iteration(s). The green LED lasts many frames; acceptable.
8. **Hardware assumption to verify:** the ES8311 locks at fs=32 kHz. On the ADV `over_sampling` is 1, the I2S port runs at 32 kHz and MCLK=BCLK. The bench measures the real chunk cadence against 31.25/s and a 440 Hz test checks the clock. Constant `kRateTrim` (1.0f).
9. **No new dependencies.** No esp-dsp. `-O3` only for `pitch_mpm.cpp` via `#pragma GCC optimize("O3")` (Arduino-ESP32 builds with `-Os`); no global flag changes.
10. **Edits outside new files:** the `main.cpp` change is 3 lines (brief said one). `onReselect` is an empty virtual, as requested.
11. **RAM:** PSRAM not needed; the ADV has none (ESP32-S3FN8, `qio_qspi`). Bench and debug print `ESP.getPsramSize()` to confirm.

| Parameter | Choice | Reason |
|---|---|---|
| Sample rate | **32 kHz** | At 16 kHz B7 has 4 samples/period and the parabola bias is ~16 c; at 32 kHz ~2 c (pure tone). The bench decides (§8). |
| WINDOW | **2048** (64 ms) | C2 (lag 489 at 32 kHz) leaves 1559 samples of overlap = 3.2 periods. |
| HOP | **1024** (32 ms) | 31.25 windows/s (> 25/s). 512 would saturate the CPU. |
| Lag sweep | **1..720** | Starting at 1 keeps pitchy's state machine exact (+0.1 %). |
| Frequency gate | 14 < f < 8200 on the untransposed pitch | As ToneMap. |
| Decimated coarse search | **Not justified** | Would change K=0.9 results vs pitchy; saves ~3 ms. Plan B. |
| Redraw | ≤ 20 fps, only on changes | §6. |

## 1. Mic_Class facts (verified in 0.2.25)

- **Two slots.** `_rec_info[2]`; consumed in submission order. `isRecording()` returns how many slots have `length != 0` (0, 1 or 2); the slot being filled counts. Released (`length.store(0)`) on writing the last sample, before the callback. `< 2` = a slot is free.
- **Buffer.** The task writes straight into the user's buffer; it must stay valid and intact until released or until `end()` returns. No alignment/length restrictions (only non-null pointer and `len > 0`). Consecutive requests are sample-continuous.
- **Type.** The `int16_t*` overload gives signed mono PCM, clipped to ±(32767−16).
- **ADV mic config:** `over_sampling=1`, `magnification=16`, 16 kHz by default, DMA 8×128 frames (= 32 ms at 32 kHz). Pins BCK 41, WS 43, DIN 46. Mono, right channel. Capture task priority 2, not pinned.
- **Rate changes.** `record(buf, n, rate)` with another rate rebuilds the port if nothing is in flight. `Mic.setSampleRate(32000)` before `Mic.begin()` builds at 32 kHz directly. The repo's `micCodecCallback` writes registers on enable and does nothing on disable: rebuilding is cheap and pop-free.
- **Gaps.** With no pending request the task parks and DMA overflows; the first request after a gap returns up to one DMA depth (32 ms) of old audio → the first chunk after any (re)start is warm-up.
- **Callback restrictions:** install/remove only before the first `record()` or after `end()`; `record()` inside the callback never waits for a slot; no `printf` inside (task stack ~2.5 KB).
- **Existing code:** `audio::micStart()` = mute, `stopAll`, `Speaker.end()`, `Mic.begin()`. `audio::micStop()` = wait up to 300 ms for `isRecording()==0`, mute, `Mic.end()`, 30 ms, `Speaker.begin()`, unmute. The classic records 2048 samples at 16000 Hz guarded by `isRecording()`.

## 2. Capture: chunk ring written directly by the mic task

- **Ring.** `int16_t ring[kChunks=4][kHop=1024]` (8 KB). Chunk n always lives in slot `n % 4`.
- **Callback.** When chunk n is released it increments `done` (`std::atomic<uint32_t>`) and, if `state==Run`, queues chunk `nextQueue++` with `Mic.record(&ring[slot], kHop, kSampleRate)`. Two chunks are always in flight.
- **Safe-window rule.** Chunks `done-2` and `done-1` are the most recent window; chunk n+4 starts being written only when n+3 finishes (`done ≥ n+4`), so the window is stable for 2 hops (64 ms). `takeWindow()` copies both chunks (~2 KB, ≈5 µs) into `win[2048]` and rereads `done`; if it advanced by more than 1 the window is torn and discarded (`torn++`).
- **Warm-up.** After any (re)start the first chunk is old: the first valid window needs `done ≥ 3`.
- **dt by sample count, not millis:** `dtMs = (seq − prevSeq) · kHopMs`, `kHopMs = 32` (32 ms normal, 64 ms if a window was skipped, as brief §4.6 says).
- **Analysis never queues:** `takeWindow()` returns only the most recent window and counts skipped ones.
- **Recovery:** `service()` (every loop) restarts the queue if `state==Run && isRecording()==0` (should not happen; covers a failed `record()` inside the callback). Restart raises `epoch`, sets `done=0` and re-applies warm-up.
- **Stop/pause:** `stop()` sets `state=Idle`; at most one more chunk is queued and in-flight ones drain in ≤96 ms. `audio::micStop()` waits for `isRecording()==0` (its 300 ms loop) before `Mic.end()`. Pause uses `stop()`; resume restarts only with `isRecording()==0`, so it never blocks.
- **Alternative (scheme A, polling):** same ring, `record()` from `service()` only if `isRecording()<2`, with `nextQueue` in `loop()`. Occasional gaps because `loop()` may exceed 32 ms.

## 3. Detector (`pitch_mpm`) and CPU

Algorithm (pitchy 4.0.1 semantics, `float` only):
1. `prepare()` computes the mean, writes `work[i] = x[i] − mean` and returns rms and peak (int16 units). The view applies the noise gate here: if the window is under the gate `findPitch()` is skipped (silence ≈ 60 µs per window instead of ~10 ms).
2. Type II NSDF for lags 1..maxLag with implicit `NSDF[0]=1`. Direct sum `r'(τ) = Σ_{j=0}^{N-1-τ} w[j]·w[j+τ]`; `m'(0) = 2·Σw²`; `m'(τ+1) = m'(τ) − w[τ]² − w[N−1−τ]²`; `n(τ) = 2r'/m'`, or 0 if `m' ≤ 0`.
3. Key maxima with pitchy's state machine from i=1 with `nsdf[0]=1`: ascending crossing `prev ≤ 0 && cur > 0` seeds `max` with that sample; descending crossing `prev > 0 && cur ≤ 0` records `maxIndex` if not −1. **Intentional difference:** at the end of the sweep, if a lobe is still open and its `maxIndex ≤ maxLag-2` with `nsdf[maxIndex+1] ≤ nsdf[maxIndex]`, it is recorded (the peak is behind). A truncated lobe whose peak was not reached is discarded.
4. `nMax` = largest key maximum; result = first key maximum ≥ 0.9·nMax (K = 0.9).
5. Parabolic refinement in shifted domain (stable in float): `δ = 0.5(ya−yc)/(ya−2yb+yc)`, `clarity = min(1, yb − 0.25(ya−yc)δ)`, `freq = fs/(lag+δ)`. Zero denominator → δ=0.

Cost model (MACs ≈ Σ_{τ=1}^{720}(2048−τ) ≈ 1.2 M):

| Kernel | Cycles/MAC | Time at 240 MHz |
|---|---|---|
| Naive, one accumulator (dependent `madd.s`, 2 loads) | ~5 | ~25 ms (misses < 20 ms) |
| 4 lags at once, 4 accumulators, 1.25 loads/MAC (**mandatory**) | 1.5–2.5 | **7–12 ms (central ~10 ms)** |

- `prepare` + key maxima + copy ≈ 0.1 ms. Per second: 31 windows × 10 ms ≈ 310 ms with sound; ≈ 2 ms in silence.
- Kernel rules: `#pragma GCC optimize("O3")` in `pitch_mpm.cpp`; all buffers static in internal DRAM.
- Resource ladder if the bench gives > 15 ms: (a) check `-O3` and the 4-lag blocking, (b) cap drawing at 15 fps, (c) maxLag 604 (53 Hz), (d) track the lag near the previous note (new risk), (e) decimate τ>256 by 2 (saves only ~30 %).
- **Expected precision (to measure):** parabola bias for pure tone ≈ 0.4 c at C7 and ≈ 2 c at B7 at 32 kHz. Borderline for ±3 c in octave 7; if it fails, documented "best effort" (brief §4.1).

## 4. Files and interfaces

```
src/pitch_mpm.h/.cpp        no Arduino; detector
src/tonemap_model.h/.cpp    no Arduino; map state + colors + notes (millis as parameter)
src/tonemap_audio.h/.cpp    capture ring + callback over M5Cardputer.Mic
src/tonemap_view.h/.cpp     drawing, keys, LED/brightness, strip, debug overlay
src/tonemap_bench.cpp       all inside #ifdef TONEMAP_BENCH
tools/gen_tonemap_vectors.py   Python stdlib only; writes test/tonemap_vectors.h
test/tonemap_vectors.h      generated, versioned
```

All headers carry: `Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)`.

### 4.1 `pitch_mpm.h`
```cpp
namespace mpm {
constexpr size_t kMaxWindow = 2048;
constexpr int    kMaxLag    = 720;           // sizing cap: nsdf needs kMaxLag+2 floats
struct Levels { float mean, rms, peak; };    // int16 units, DC removed
struct Result { bool found; float freq, clarity, lag; };
Levels prepare(const int16_t* x, size_t n, float* work);   // work[n]
Result findPitch(const float* work, size_t n, float sampleRate, int maxLag, float* nsdf);
inline Result detect(const int16_t* x, size_t n, float fs, int maxLag, float* work, float* nsdf); // bench
}
```

### 4.2 `tonemap_audio.h`
```cpp
namespace tonemap_audio {
constexpr uint32_t kSampleRate = 32000;
constexpr size_t   kWindow = 2048, kHop = 1024, kRingChunks = 4;
constexpr uint32_t kHopMs = 32;
constexpr float    kRateTrim = 1.0f;         // calibrate at STOP A
void attach();                               // setSampleRate(32000) + install callback; mic must be ended
void detach();                               // clear callback + setSampleRate(16000); after Mic.end() returned
bool start();                                // requires audio::micOn(); queues chunks 0 and 1; epoch++; done=0
void stop();                                 // state=Idle
bool running();
void service();                              // every loop (starvation restart, pending resume)
struct WindowInfo { uint32_t seq, epoch; };
bool takeWindow(int16_t* dst /*2048*/, WindowInfo& info);   // newest complete window not yet taken
struct Stats { uint32_t chunks, starved, torn, skipped, recFail; };
Stats stats();
}
```
The callback checks that `data` is inside the ring and `len == kHop`; ignores the rest. Uses only the file's static state, not `args`.

### 4.3 `tonemap_model.h`
```cpp
namespace tonemap {
constexpr int kNoteCount = 128;              // indexed by CONCERT midi, 0..127
constexpr int kGridLo = 36, kGridHi = 107;   // WRITTEN midi shown (C2..B7)
enum class Sensitivity : uint8_t { Relaxed, Medium, Hard };
struct SensConst { float deadzone, errScale, spanMul, needleTauMs; };
extern const SensConst kSens[3];             // {0.0025,0.7,4,350} {0.0021,0.7,4,80} {0.00105,0.9,8,30}  (2026-10-22: spanMul 2/2/4, see §13)
constexpr uint32_t kHistoryMs[4]  = {1000, 3000, 30000, 0};            // 0 = unlimited
constexpr int32_t  kHoldMs[6]     = {5000, 15000, 30000, 120000, 300000, -1};  // -1 = no fade
constexpr int8_t   kTransposeSemis[4] = {0, -2, 3, 5};
struct Rgb { uint8_t r, g, b; };             // (2026-10-22: Safe / Extreme palettes (formerly Classic) instead of kHigh/kCenter/kLow, see §13)

// The single place a temperament would plug in.
float latticeOffsetCents(int concertMidi);                       // 0 in v1
float targetOffsetCents(int concertMidi, float a4);              // lattice + 1200*log2(a4/440)
float targetHz(int concertMidi, float a4);                       // 440*2^((m-69)/12)*2^(off/1200)
struct NoteHit { int concertMidi; float cents, errRatio, targetHz; };
NoteHit freqToNote(float hz, float a4);       // ±2 candidates around round(); minimise |(pos-cand)*100 - off|; ties -> lower
float   needlePct(float cents);               // (clamp(c,-50,50)+50)/100
Rgb     biasColor(Sensitivity s, float avgErrRatio);  // 2026-10-22: biasT + paletteColor + setPalette, see §13
bool    inDeadzone(Sensitivity s, float errRatio);
float   baseIntensity(uint32_t totalMs);      // min(1, total/500)  (2026-10-22: total/250, see §13)

struct TickResult { bool hasNote, newNote; int concertMidi, writtenMidi; float hz, cents, displayCents, errRatio; bool inDeadzone; };

class Model {
public:
  void reset(uint32_t nowMs);
  void setSensitivity(Sensitivity, uint32_t nowMs);    // recompute: lastStain=now, anchor cleared
  void setHistory(int idx, uint32_t nowMs);             // same
  void setRefA(float a4, uint32_t nowMs);               // same (rescoring is implicit, see §5.4)
  void setTransposition(int semis, uint32_t nowMs);     // ends the active note; same recompute
  void setHold(int idx);                                // no timer restart
  TickResult tick(bool valid, float hz, uint32_t dtMs, uint32_t nowMs);
  void endNote();                                       // pause / leave
  Rgb  cellColor(int writtenMidi, uint32_t nowMs, bool* visited);  // evaluates fade; black if display <= 0.001
  bool anyFading(uint32_t nowMs) const;
  // test hooks
  float    avgErrRatio(int concertMidi) const;
  uint32_t totalMs(int concertMidi) const;
  float    displayIntensity(int concertMidi) const;
};
}
```
A `static Model` lives in `tonemap_view.cpp` (.bss, zero state is valid), so the map survives mode and view changes.

### 4.4 `tonemap_view.h` (used by `mode_tuner.cpp`)
```cpp
namespace tonemap {
void enter();  void leave();                  // leave() idempotent
void onKey(const KeyEvent&);
void update();
bool draw(M5Canvas&, bool force);
void afterPush();
}
```

## 5. Map state and bytes

### 5.1 Stored range
Indexed by **concert MIDI 0..127** (128 notes; matches ToneMap's range and loses nothing on transposing).

| Range | Notes | Bytes |
|---|---|---|
| **0..127 (recommended)** | 128 | **37,888** |
| 24..119 (if heap is short) | 96 | 28,416 |
| 36..107 (not recommended: loses data with K) | 72 | 21,312 |

The detector can only produce concert MIDI ≈ 29..121, but 0..127 fits the budget.

### 5.2 `NoteState` (`static_assert(sizeof==296)`)
```
offset size field
  0     8  int64  sumAll           Σ dur_ms * err_q16 over all voiced time (Unlimited)
  8     4  uint32 totalMs          voiced ms since reset (saturating)
 12     4  uint32 lastStainMs
 16     4  float  displayI         stored display intensity
 20     4  float  fadeAnchor
 24     4  int32  accFineSum       open fine chunk: Σ dur*err
 28     4  int32  accCoarseSum     open coarse chunk
 32     2  uint16 accFineMs
 34     2  uint16 accCoarseMs
 36     1  uint8  fineHead, 37 fineCnt, 38 coarseHead, 39 coarseCnt
 40     1  uint8  flags            bit0 visited, bit1 anchorValid, bit2 everStained
 41     1  pad
 42   136  Chunk fine[34]          Chunk = { uint16 durMs; int16 errQ16 }  (4 B)
178   116  Chunk coarse[29]
294 -> sizeof 296 (align 8)
```
- **Total:** 128 × 296 = 37,888 B + Model globals (parameters, active note, needle EMA, A4) ≤ 64 B ≈ **37,952 B = 37.06 KiB**, under 40,960 B with 3,008 B margin.
- `errQ16 = round(errRatio · 65536)` in `int16` (range ±0.5). Stored errors are relative to the A=440 target (§5.4), worst case ±(0.03·1.09 + 0.09) ≈ ±0.125.
- Zero state = valid empty map; `fadeAnchor` has a validity flag, not a −1 sentinel.
- **Other RAM** (not map state): ring 8,192 + `win` 4,096 + `work` 8,192 + `nsdf` 2,888 ≈ 23.4 KB, static DRAM. The 60 KB canvas already exists.

### 5.3 Fine/coarse chunks and History
- **Adding a frame** (valid, same note as the previous valid frame, `dt>0`): `errQ = clamp(round(stored · 65536))`, then `accFineSum += dt·errQ; accFineMs += dt; totalMs += dt; sumAll += dt·errQ`. If `accFineMs ≥ 96` the fine chunk closes: `{ms, round(sum/ms)}` is pushed to the fine ring. Chunks of 96 ms (three 32 ms frames), longer only if windows were skipped.
- **Cascade.** When the fine ring (34 entries, ≥ 3.26 s) is full, the oldest entry is poured into the open coarse accumulator. At `accCoarseMs ≥ 1000` it closes into the coarse ring (29 entries ≈ 30.6 s). Total coverage ≥ 32.2 s.
- **History query** of H ms, newest to oldest: open fine accumulator → fine ring → open coarse accumulator → coarse ring. For each element `take = min(dur, remaining)`; add `take·avg`; `remaining -= take`. The edge chunk enters fractionally (like ToneMap's `Math.min(seg.durMs, remaining)`); error bounded by one chunk (≈ 96–130 ms), allowed by brief §4.4. Total duration 0 → returns 0.
- History 1 s and 3 s: only the first two sources. 30 s: all four. **Unlimited:** `sumAll / totalMs`, exact.
- **Display state:** `visited`, `displayI`, `fadeAnchor` and `lastStainMs` follow ToneMap's `updateCellDisplayColor` (`index.html` L10252-10324). Deviation: Relaxed's EMA fade-in uses Δ = tick dt. Fade evaluation is lazy and deterministic, but the value is materialized into `displayI` before any stain, recompute or reset (ToneMap rewrites it every frame). Medium/Hard: `display = base`. Relaxed: EMA toward base only on the stain frame. Fade: after `hold`, a 10 s linear ramp from the anchor to `0.25·base` (since 2026-10-22 `0.45·base`, see §13), then it stays; no fade = no ramp. `base = 0` or never stained → `display = 0`.

### 5.4 A4 re-basing and transposition
- **Re-basing without rewriting chunks.** The stored error in each chunk is relative to the A=440 target: `stored = (1+errRatio_actual)·(a4/440) − 1`. On read: `e_cur = (1+avgStored)·(440/a4_cur) − 1`. It is ToneMap's same affine re-basing (`rebase(err, from, to)` with `from−to = 1200·log2(a4_old/a4_new)`), applied once to the average; exact because all chunks share the reference. No quantization drift after pressing Up/Down many times (the bench tests it). `setRefA()` applies the side effects of ToneMap's recompute (`lastStain=now`, anchor cleared, Medium/Hard jump to base). **Limitation:** a future temperament with per-segment lattice offsets would need a per-chunk offset or in-place re-basing.
- **Transposition.** Notes are stored by concert MIDI. The grid shows the written note `w = concert − t`, i.e. `c = w + t`. Changing `t` only changes the lookup (equivalent to `shiftCellStateBySemitones(old−new)`). `setTransposition` ends the active note and applies the recompute effects. Frequency→note uses the untransposed pitch and the concert target; algebraically equal to ToneMap's `adjustedPitch` path (the `2^(−t/12)` factors cancel).

## 6. Loop schedule and CPU budget

Inside the existing `loop()`; `main.cpp` changes only 3 lines:
```
loop():  M5Cardputer.update(); input::poll()                               ~0.3 ms
         keys -> handleKey -> TunerMode::onKey -> tonemap::onKey
         settings::tick(); audio::update()
         TunerMode::update() -> tonemap::update():
              LED resends / expiry, message expiry
              tonemap_audio::service()
              if (!paused && takeWindow()):                                (new 32 ms window)
                  prepare (60 us) -> gate -> findPitch (~10 ms) -> model.tick (~20 us)
         draw(): only if dirty && now-lastDraw >= 50 ms (20 fps); if an analysis ran
                 this iteration, defer unless overdue > 80 ms     -> build frame ~4 ms
         main: pushSprite + waitDisplay ~13-15 ms -> afterPush() (measure push time)
         delay(4)
```

| Item per second (20 fps, 31 windows/s) | ms |
|---|---|
| NSDF analysis (31 × ~10 ms) | ~310 |
| `prepare` (31 × 0.06 ms) | 2 |
| model ticks | < 1 |
| drawing (CPU, 20 × ~4 ms) | ~80 |
| push + `waitDisplay` (20 × ~14 ms) | ~280 |
| `delay(4)` and keyboard | ~30 |
| **Total busy** | **~700 of 1000** |

- The naive 25 ms kernel would overrun (~1.1 s): the 4-lag kernel is mandatory.
- Nothing is drawn if nothing changed (in silence with no fades it drops to ~0).
- **"Dirty" events:** new note, cents or Hz with a visible change at 0.1, needle movement in pixels, level meter movement in pixels, message start/end, a key, and a 500 ms timer only while `anyFading()` is true.
- **Optional optimization if the push limits:** `Display.setClipRect()` in `draw()` to push only the strip and changed rows, and `clearClipRect()` in `afterPush()` and `leave()`. Needs no `main.cpp` change, but `drawBar()` must never run with a clip set.
- **Level meter:** `frac = clamp((20·log10(rms/32768)+60)/54, 0, 1)`; the threshold mark uses the same mapping.
- **Geometry** (as the brief): strip text at y=3 (Font0); separator y=15; grid y=17..124 (6 rows × 18 px), 12 columns × 20 px; cell lines on the right and bottom (`#374151`). Active cell: inner white 2 px border, black 2 px reference line at the vertical center, red `#ef4444` needle 2 px at `top = 90% − pct·80%`. Strip cents and Hz show the displayed cents (smoothed in Relaxed, raw in Medium/Hard); green and the LED use raw per-frame `|errRatio| ≤ DEADZONE`.

## 7. Integration

### 7.1 `ui.h`
```cpp
// Called when the mode's own hotkey is pressed while it is already active.
virtual void onReselect() {}
```

### 7.2 `main.cpp` (`handleKey`)
```cpp
for (Mode* m : modes) {
    if (m->hotkey() != e.ch) continue;
    if (m == current) return current->onReselect();
    return switchTo(m);
}
```
`switchTo(next==current)` was a no-op, so other modes behave the same (their `onReselect` is empty). The key never reaches `onKey`: the metronome's tap measurement is unaffected.

### 7.3 `settings.h` / `settings.cpp`
```cpp
int tunerView = 0;     // 0 classic, 1 tonemap
int tmSens   = 1;      // 0 relaxed 1 medium 2 hard
int tmHist   = 1;      // 0 1s 1 3s 2 30s 3 unlimited
int tmHold   = 2;      // 0 5s 1 15s 2 30s 3 2min 4 5min 5 no-fade
int tmGate   = 2;      // new scale (§16): 0 none 1 min 2 low 3 med 4 high; NVS key tmGateB (old tmGate: 0 none 1 low 2 med 3 high)
int tmTransp = 0;      // semitones: 0, -2, 3, 5
int tmFlats  = 0;
int tmLed    = 1;
int tmPalette = 0;   // 2026-10-22: 0 safe, 1 extreme (formerly classic; see §13)
```
NVS keys: `tunerView`, `tmSens`, `tmHist`, `tmHold`, `tmGate`, `tmTransp`, `tmFlats`, `tmLed`, and since 2026-10-22 `tmPal` (≤ 15 chars). `clampAll`: `constrain` each; `tmTransp` forced to 0 if not in {0, −2, 3, 5} (storing semitones makes adding A (−3) trivial). `load()`: `getInt` with the current value as default. `tick()`: `putInt`. The loaded-settings serial line is extended. Deferred saving unchanged; `touch()` on every change.

### 7.4 `mode_tuner.cpp` (classic code unchanged, only moved)
- Include `tonemap_view.h`; add member `bool tm_`. The current bodies of `enter()`/`leave()` become private `enterClassic()`/`leaveClassic()`, identical.
```cpp
void enter() override {
    tm_ = settings::get().tunerView == 1;
    if (tm_) tonemap::enter(); else enterClassic();
}
void leave() override { if (tm_) tonemap::leave(); else leaveClassic(); }
void onKey(const KeyEvent& e) override { if (tm_) return tonemap::onKey(e); /* classic body */ }
void update() override { if (tm_) return tonemap::update(); /* classic body */ }
bool draw(M5Canvas& c, bool force) override { if (tm_) return tonemap::draw(c, force); /* classic */ }
void afterPush() override { if (tm_) tonemap::afterPush(); }
void onReselect() override {
    if (tm_) { tonemap::leave(); tm_ = false; settings::get().tunerView = 0; settings::touch(); enterClassic(); }
    else     { leaveClassic(); tm_ = true; settings::get().tunerView = 1; settings::touch(); tonemap::enter(); }
    dirty_ = true;
}
```
The classic `update()` must not run with ToneMap active: its `pollMic` would fight the chunk queue.

### 7.5 Enter/leave matrix (mic, drone, brightness, LED)

`tonemap::enter()`, in order:
1. `tonemap_audio::attach()` (32000 Hz, installs callback; the mic is ended on every entry path).
2. `audio::micStart()`.
3. Save the user's brightness; if LED is on, raise to 255 if lower and remember it was raised.
4. Sync the model with settings, silently (no timer restart if nothing changes).
5. `tonemap_audio::start()`.
6. If `micStart` failed: show "Microphone unavailable" and skip capture.

`tonemap::leave()` (idempotent), in order:
1. `tonemap_audio::stop()`.
2. If the LED was on: send off three times with `delay(6)` between sends (the pin is still at 255).
3. Restore brightness (only if raised).
4. `audio::micStop()`.
5. `tonemap_audio::detach()` (rate back to 16000).
6. `model.endNote()`.

| Path | Drone | Mic | Brightness / LED |
|---|---|---|---|
| Other mode → Tuner (ToneMap) | off (classic `leave()` already stopped it) | `micStart` | raise if LED on |
| Other mode → Tuner (classic) | unchanged | unchanged | unchanged |
| ToneMap → other mode (M, G, A) | n/a | `leave()` stops capture and `micStop` | LED off, restore, **before** the next mode's `enter()` reads brightness (`switchTo` calls `leave()` then `enter()`) |
| ToneMap → classic (T) | n/a | full `leave()` then `enterClassic()` | restore |
| Classic with drone → ToneMap (T) | `leaveClassic()` runs `droneStop()` (fade-out) and `micStop()` | `tonemap::enter()` does `micStart` | raise if LED on |
| `L` on | n/a | n/a | save brightness, raise to 255 |
| `L` off | n/a | n/a | LED off (with resends), restore brightness |
| `Space` pause | n/a | capture stopped, mic stays on | LED off, brightness unchanged |
| `Space` resume | n/a | restarts with `isRecording()==0` | unchanged |
| `micStart` fails | n/a | no capture | LED/brightness symmetric |
| Capture starvation | n/a | `service()` restarts, epoch++ | unchanged |

Classic↔ToneMap go through the full `leave()`/`enter()` of the side being left: same proven mic path as drone on/off; costs ~300 ms on pressing `T`.

### 7.6 ToneMap keys

| Key | Handler |
|---|---|
| `S` | rotates `tmSens`; `model.setSensitivity`; message |
| `H` | rotates `tmHist`; `model.setHistory`; message |
| `C` | rotates `tmHold`; `model.setHold`; message |
| `N` | rotates `tmGate`; message |
| `K` | rotates transposition; `model.setTransposition`; message |
| `F` | toggles `tmFlats`; message |
| `L` | toggles `tmLed`; brightness/LED as above; message |
| `P` | (2026-10-22) toggles `tmPalette`; `model.setPalette`; message `Palette: Safe` / `Palette: Extreme` |
| `R` | `model.reset`; message |
| `Space` | pause/resume (map clock stops) |
| Up/Down | `refA ± 1` (400..480); `model.setRefA` |
| Left/Right | unused |

Each change calls `settings::touch()`. 1.5 s messages (`Sensitivity: Hard`, `History: 30 s`, `Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`, `LED: off`, `Map cleared`). While paused the strip shows fixed `Paused (Space)`.

**Gate table (calibrated at STOP A, see §12; since 2026-10-23 with one more `Min` level, see §16):** `kGateRms = {0, 30, 80, 200, 450}` (None, Min, Low, Med, High), `kMinClarity = 0.8`. Before: `{0, 80, 200, 450}`. (Initial design value: `{0, 120, 300, 800}`.) The energy check is `sumSq > 0` (equals ToneMap's `rms>1e-7 || peak>4e-6` in int16 units).

## 8. TONEMAP_BENCH and TONEMAP_DEBUG

```ini
[env:cardputer-adv-tonemap-bench]
extends = env:cardputer-adv
build_flags =
    ${env:cardputer-adv.build_flags}
    -DTONEMAP_BENCH=1
    -I${PROJECT_DIR}/test
build_src_filter =
    +<*>
    -<main.cpp>
    -<mode_*.cpp>
    -<tonemap_view.cpp>
    -<input.cpp>

[env:cardputer-adv-tonemap-debug]
extends = env:cardputer-adv
build_flags =
    ${env:cardputer-adv.build_flags}
    -DTONEMAP_DEBUG=1
```
`tonemap_bench.cpp` provides `setup()`/`loop()` in the bench build and compiles to an empty unit elsewhere. All serial text in English.

### 8.1 Bench output
```
[bench] ToneMap bench, build <date> <time>
[bench] cpu=240MHz heap_free=... heap_max_block=... psram=0
[bench] sizeof(NoteState)=296 sizeof(Model)=... budget=40960
[synth] fs=32000 N=2048 maxLag=720 wave=saw f=440.00 off=+20c det=440.26 err=+0.01c clar=0.99 oct=ok PASS
...
[synth] SUMMARY fs=32000: 60 cases, octave errors 0, max|err| <=C6 0.4c, oct7 1.9c PASS
[synth] SUMMARY fs=16000: ...
[time] fs=32000 maxLag=720: avg 9.8 ms max 10.4 ms (50 runs, saw C3)
[time] fs=16000 maxLag=360: avg ...
[test] parity equal/none: 264/264 PASS
[test] parity extra: n/n PASS
[test] color / history / fade / rebase / transpose / chunks / first-frame / nan / budget ... PASS
[test] SUMMARY: ALL PASS | FAILURES=k
[live] note=A4 cents=+3.2 hz=442.6 clar=0.97 rms=1234 an/s=30.1 chunks/s=31.2 loop_ms=33.2 starved=0 torn=0
```
- **Synthetic cases:** band-limited sine and sawtooth (additive harmonics by rotation recurrence, no `sinf`), at 16 and 32 kHz. Notes: C2, A2, C3, C4, A4, C5, C6, C7, A7, B7, each at −20, 0, +20 c. Rule: no octave error; ≤ 1 c up to C6; ≤ 3 c in octave 7 (or documented "best effort").
- **Timing:** 50 analyses per rate, mean and max in µs.
- **Live:** ~10 lines/s of the above. A key (`d`) simulates drawing load (240×125 sprite push every 50 ms) to measure realistic `an/s`. A 10 s measurement of `chunks/s` against 31.25 gives the real I2S clock error, which sets `kRateTrim`.

### 8.2 Debug environment
- One overlaid line over the last row (y=117, black background): `a/s 30.8 an 9.6ms dr 4.1 push 13.7 rms 412 cl 0.93`.
- Serial twice per second: `[tm] an/s=30.8 an_ms=9.6/10.9 draw_ms=4.1 push_ms=13.7 fps=19.8 loop_ms=32.4 rms=412 clar=0.93 hz=441.2 note=A4 +3.1c gate=Low starved=0 torn=0 skipped=2 heap=... minheap=...`.
- Push time is measured between `draw()` returning and `afterPush()`.

## 9. Unit and parity tests

**Generation** (`tools/gen_tonemap_vectors.py`, Python stdlib):
- Input: `refs\tonemap.live\tests\fixtures\temperament-vectors.json` (object with `rows[]`, 5,192 rows, 264 `equal`/`none`).
- Output: `test/tonemap_vectors.h`, versioned; the build never needs the external clone.
- Emits `ParityRow {float hz, a4; int16 midi; float cents, needle;}` for the 264 rows (`inputHz, a4, expect.midi, expect.cents, expect.needleEven[0]`): the "264/264" criterion is literal.
- Emits extra rows computed in double precision with ToneMap's formulas (candidate search, ties to lower): negative cents, ±49.9, ±50.1 (belongs to the next note), exact ties avoided; needle clipping at ±60 c; transpositions (0, −2, 3, 5); extreme MIDI (0, 12, 107, 119, 127); A4 in {415, 440, 442, 466}.
- **Guard:** before emitting extras, the script checks its formulas reproduce the 264 fixture rows to 1e-9, anchoring the extras to the real fixture.

**Execution:** the bench runs everything at boot and prints `PASS`/`FAIL` per series. `pio run -e cardputer-adv-tonemap-bench`; Sergio installs `firmware.bin` (Launcher) or it is flashed over USB with his permission; Claude reads serial. There is no host `gcc` on this machine, so no native test environment.

**Criteria:**
- **Parity:** exact `midi`; |cents − expected| ≤ 0.01; |needle − expected| ≤ 0.001.
- **Color** (±1 per channel): Medium: 0 c → center; 5.2 c → center (limit); 15.6 c → t=0.5; ≥ 26 c → saturated high (255,170,0); −40 c → saturated low (0,86,214). Hard: center up to 2.0 c; saturated at 18.2 c. Relaxed: center up to 6.2 c; saturated at 30.9 c. Plus blend over black at display 0.5.
- **History:** 1 s at +10 c then 2 s at −10 c → H=1 s: −10 c; H=3 s: −3.33 c; H=30 s and Unlimited: −3.33 c. 35 s at +10 c and 5 s at −10 c → H=30 s: +6.67 c; Unlimited: +7.5 c; H=3 s: −10 c (exercises ring eviction). Tolerance ≈ one chunk's weight (~1 c at H=3 s, ~0.1 c at H=30 s). Unlimited exact to 1e-6.
- **Fade** (hold 5 s, base=1): 1.0 at +4 s; 0.625 at +10 s; 0.25 at +20 s; no fade stays at 1.0. Relaxed EMA fade-in after n ticks = `1−(1−α)^n`.
- **Re-basing:** A4 442→440 with history at +0 c → +7.85 c (±0.02); 440→442 → −7.85; extremes 400/480; 100 Up/Down presses return exactly (no drift).
- **Transposition:** C→Bb moves the concert A4 cell to written 71 (B4); Eb → 66; F → 64; C→F→C restores colors.
- **Chunk equivalence:** 120 s of pseudo-random frames against a brute-force reference in bench-only RAM; H=1/3/30 s within the edge-chunk bound; Unlimited exact.
- **First-frame rule:** n valid frames of the same note produce n−1 segments; an invalid frame ends the note; a note change produces no segment on its first frame.
- **Robustness:** `hz = 0 / NaN / inf` with `valid=true` and `false`; `dtMs=0`; reset clears everything.
- **Budget:** `static_assert(sizeof(NoteState)==296)` and `sizeof(Model) ≤ 40960`.
- **Detector extras:** silence → `found=false`; white noise → clarity < 0.8 (no false notes).

## 10. Implementation order

1. Phase 3: `pitch_mpm`, `tonemap_audio`, bench env with synthetic, timing and live sections. Decide 16 vs 32 kHz from the data and note it here (§12).
2. Phase 4: `tonemap_model` + `tools/gen_tonemap_vectors.py` + `test/tonemap_vectors.h` + model tests in the bench.
3. Phase 5: `tonemap_view` and the 7 small changes (`ui.h`, `main.cpp`, `settings.h/.cpp`, `mode_tuner.cpp`, `platformio.ini`). Build all envs, commit and push after each successful build (`CLAUDE.md`).

## 11. Orchestrator decisions

- The design **does not contradict §2** or add dependencies → no stop after phase 2.
- Needle in Medium/Hard unsmoothed (faithful replica of ToneMap, brief §3.6).
- Pause: the map clock stops (fades freeze) — consistent with "the map freezes" of §2.5.
- The `main.cpp` change is 3 lines instead of one; considered within the spirit of §4.7.
- The callback scheme (§2) is used; polling stays as an alternative.

## 12. Measured values

### Phase 3 (bench on the Cardputer ADV, 2026-10-17)

**Decision: 32 kHz, WINDOW 2048, HOP 1024, maxLag 720.**
- **I2S clock at 32 kHz:** 31.2495 chunks/s (nominal 31.25), `ratio = 0.99999` → `kRateTrim = 1.0`. The ES8311 locks fine. `starved=0`, `torn=0`, `recfail=0`, `badcb=0`. The first ~1.5 s of capture yield few chunks (warm-up).
- **Time per analysis (32 kHz, maxLag 720):** **9.25 ms** (mean and max 9.26 ms, 50 runs), matching the design estimate. Time does not depend on the signal (direct sum, fixed cost). `prepare` 0.2 ms. Window under the noise gate: 0.2 ms. At 16 kHz / maxLag 360: 5.2 ms.
- **Live, silent room, with simulated draw load (240×125 sprite every 50 ms):** 31.0 analyses/s (`skipped=2` when the load starts), screen push 12.1 ms, draw 2.3 ms, `loop_max` 19 ms. Still to measure with full analysis (9 ms) + real drawing: STOP A / phase 5.
- **Synthetic precision (A4=440, N=2048, ±20 c):**

| fs | Wave | Octave errors | Up to C6 | Octave 7 |
|---|---|---|---|---|
| 32 kHz | sine | 0 | ≤ 0.05 c | **≤ 1.65 c** (meets ±3 c) |
| 32 kHz | sawtooth | 0 | ≤ 0.14 c up to C5; C6: −0.45 / −0.96 / **+1.39 c** | C7 +1.6 / +4.6 / +2.5 c; A7 +8.7 / +4.7 / −0.7 c; B7 +7.6 / +4.5 / +0.2 c |
| 16 kHz | any | **7** (octave 7) | up to 4.6 c | up to 11 c |

  16 kHz is ruled out (octave errors in octave 7). At 32 kHz, C2–C5 is exact (< 0.15 c).
- **Outside criterion (brief §4.1 and §6 A):** with sawtooth (worst case: 1/n harmonic amplitudes, narrow NSDF peaks) octave 7 is **biased upward up to +8.7 c**, and C6 +20 c gives +1.39 c (criterion ≤ 1 c). Sine meets everywhere. The bias is systematic and comes from pitchy's parabolic interpolation on narrow peaks (the Python prototype predicted the same); in web ToneMap at 44.1/48 kHz it is smaller. **Accepted as "best effort" in octave 7 for harmonic-rich timbres** and reviewed with real violin/flute at STOP A. If it bothers, the fix (deviating from pitchy) would be higher-order peak interpolation or oversampling around the peak.

**Serial port note:** with USB-CDC (HWCDC) the bench needs an explicit `Serial.begin(115200)` for `Serial.print` to appear; the port re-enumerates after every reset, so the bench waits up to 30 s for the host to open it. System logs (`E (...)`) appear even if not opened.

### Phase 4 (bench on the Cardputer ADV, 2026-10-17)
- `[test] SUMMARY: ALL PASS | FAILURES=0` (14 series, ~1000 checks; the test phase takes 1028 ms). Parity with the ToneMap fixture **264/264** (max |Δcents| 0.00021, |Δneedle| 2e-6) and 192/192 extra rows (negative cents, ±49.9/±50.1, needle clipping, transpositions, extreme MIDI, A4 415–466).
- `sizeof(NoteState)=296`, `sizeof(Model)=37912` (budget 40960). `psram=0`; free heap with bench: 262 KB.
- Rebase 442→440: +7.854 c (expected +7.851 c), 160 Up/Down presses without drift.
- Chunk equivalence vs brute force: max deviation 5.6e-4 (H=1 s), 1.9e-4 (3 s), 6.3e-5 (30 s) on random flow.
- Model deviations from the design (declared by the implementer): `setHold(idx, nowMs)` and `displayIntensity(i, nowMs)` take `nowMs`; a frame contributes at most 2000 ms; the History mean is done in the error-ratio domain (like ToneMap), hence −3.3076 c and not −3.3333 c in the 1 s/2 s case; `anyFading` is true until `hold + 10 s + 600 ms`; `fadeAnchor` is unused; frequency→note rejects the frame if the best candidate is over 100 c away; `-I${PROJECT_DIR}/test` became `-Itest` (the `${PROJECT_DIR}` path broke on Windows).
- `Model` has an empty constructor so it lives in `.bss`; never create it on the stack.

### STOP A: calibration with viola (2026-10-19, Cardputer mic at ~30 cm)
Method: the bench dumps raw audio (key `w`, 32 kHz, 1024-sample chunks in base64, 135 s without gaps) and the detector is rerun offline in Python (`mpm_off.py`, same NSDF and maxima machine as the C++) with different parameters.
- **Levels (rms int16, 2048 windows):** silence median 5, max 12; speaking voice median 26, p99 79, max 86 (a first, louder test had voice peaks of 640); viola median 190, p90 509, max 1043. High notes (C6–C7) and pianos give rms 90–200.
- **Noise gate:** with clarity > 0.8, voice leaks 105/233 frames with no gate, 18/233 at 60, 3/233 at 80 and 0/233 at 120; silence never. Valid viola frames kept: 100 % no gate, 88 % at 80, 81 % at 120, 76 % at 150, 37 % at 300. **Chosen table: None 0, Low 80 (default), Med 200, High 450.**
- **Clarity:** raising from 0.8 to 0.9 loses ~4 % of valid viola frames and adds nothing with the gate on → **kept at 0.8** (ToneMap's value).
- **Octave errors in the bass (viola):** the mic loses the fundamental of the low strings. With K=0.9 (pitchy): F3 comes out as F4 (clarity 0.95), and in pizzicato E3/D3/C3 come out as E4/D4/C4; the first live capture also had C2/G2 and D3/D4 alternations when tuning. K sweep on the recording: K = 0.8–0.9 same; K = 0.95 fixes F3 only 28 %; K = 0.97 88 %; K = 1.0 is chaotic (G2, A#1…). K = 0.97 over the whole range spoils ~17 high frames (A6→A5, F6→F5, E6→E5). **Decision: hybrid K** — K = 0.9 and, if the candidate is below 400 Hz, pick again with K = 0.97 (`kLowK`, `kLowFreqHz` in `pitch_mpm.cpp`). On the recording only low frames change and all changes are corrections (F4→F3 ×138, E4→E3 ×14, D4→D3 ×11, C4→C3 ×2); highs are untouched. It is a **deviation from pitchy**. 32 kHz synthetic bench regression: identical to before.
- **Open strings (hybrid K):** C3 +4 c, G3 +3 c, D4 +9 c, A4 +7 c vs A=440 (A=442 ≈ +8 c): stable tuning, ±1–2 c deviations during the sustained note (clarity 1.00).
- Pending: confirm live with the ToneMap view (STOP B) and, if needed, another `kLowFreqHz` for flute.

## 13. Changes after device tests (2026-10-22)

Sergio used the view on the Cardputer ADV (phases 0-5 done and tested) and asked for four changes. Several are **deliberate deviations from tonemap.live** to make the map read better at a glance; they are marked as such in the code comments (`tonemap_model.h/.cpp`).

### 13.1 Active cell marker
The inner white 2 px border covered too much of the 20 × 18 px cell. First replaced by four L corners; after the second test batch (2026-10-22) **the corners were removed** and white became exclusive to the active cell, in two places and with no outline or shadow:
- **White bar** (`drawActiveCell`, `kActiveBarH` = 2): `fillRect(x, y + kCellH - 1 - kActiveBarH, kCellW - 1, kActiveBarH)`, i.e. columns x..x+18 (inside the right grid line, x+19) and rows y+15 and y+16, resting on the bottom grid line (row y+17). Drawn before the black reference line and the red needle, so the needle shows where it crosses.
- **White label** (`drawCell`): the active cell (`hasNote && writtenMidi == midi`) draws its name in white even if its color is light; when no longer active it returns to the usual rule. Other visited cells: black or **light gray `(190,190,190)`** (`kColLabelLight`) per `useBlackLabel`; never-touched ones, `#4b5563`. No other white in the grid.
- The black reference line (x+2..x+17, 2 px at the vertical center) and the needle do not change. The label (rows y+2..y+8) shares row y+8 with the black line, as before.
- **Known risk:** over the near-white center of Safe `(200,255,200)` the white bar and label contrast little.

### 13.2 Palettes and color blindness
The model still computes the trend as before, but now exposes it as `float biasT(Sensitivity, avgErrRatio)`: signed `t` in [−1, +1] (+ = high), 0 inside the dead zone or with NaN. `Rgb paletteColor(int palette, float t)` turns it into color; `Model::setPalette(int)` stores the palette (0 Safe, 1 Extreme; out of range is clamped) and `cellColor` uses it. Changing palette only needs a redraw: no data or timers touched (`setPalette` takes no `nowMs`). `biasColor(palette, s, avgErrRatio)` stays as shortcut for `paletteColor(palette, biasT(...))`.

| t | Safe (0) | Extreme (1), with t = linear position (§13.7) |
|---|---|---|
| −1 | (0, 100, 255) | (0, 0, 255) |
| −0.5 | (100, 178, 228) | (0, 255, 255) |
| 0 | (200, 255, 200) | (0, 255, 0) |
| +0.5 | (228, 183, 100) | (255, 255, 0) |
| +1 | (255, 110, 0) | (255, 0, 0) |

- **Safe:** blue – near white – orange, linear RGB interpolation center→high and center→low. The blue/orange axis survives protanopia and deuteranopia (which confuse red and green), and for tritanopia (blue-yellow) luminance counts most. Also **luminance is strictly ordered** center (0.94) > high (0.52) > low (0.35) (relative luminance over encoded channels, Rec. 709 weights), so information does not depend on hue alone: works in grayscale and when hue contrast fails.
- **Extreme (formerly Classic):** Sergio's original idea (green center, red high, blue low) with max-luminance midpoints (yellow, cyan) so the gradient does not darken halfway. The least safe option for color blindness: with protanopia/deuteranopia green and yellow/orange look very similar.
- **Approximate check** (Machado et al. 2009 matrices at severity 1, written from memory, ΔE76 in CIELAB; indicative, not clinical, not made with an independent simulation tool): Safe keeps ΔE ≥ 47 between its three points in normal vision, protanopia, deuteranopia and tritanopia (min 47 low–center with tritanopia, 54 center–high with protanopia). Extreme drops to ΔE 8 (protanopia) and 20 (deuteranopia) between green and yellow, and to 17 between cyan and green with tritanopia. The old ToneMap palette had ΔE 24 between center and high with protanopia.
- **Labels and markers:** black or white per `useBlackLabel(Rgb)` (`relLuminance` > 0.5, pure function tested in the bench). Luminance is computed over **encoded** sRGB channels (not linearized): with the 0.5 threshold on linear luminance, orange (0.32) would get white text with worse contrast. With the current rule: black text over Safe's center, full orange (0.521, just above threshold: when dimmed it turns white) and Extreme's centers, yellows and cyans; white over blue, red and dark cells. Never-touched cells keep the gray `#4b5563` label on black.
- **Setting:** `tmPalette` (0..1, default 0, NVS key `tmPal`), `clampAll`/`load`/`tick`, and the `[settings] tuner:` serial line carries `pal=%d`. Key `P` only in the ToneMap view (not global), 1.5 s message `Palette: Safe` / `Palette: Extreme`.
- **Known risk:** the needle (`#ef4444`) is barely distinguishable over Extreme's full red (high) and poorly over Safe's orange; the needle was left untouched on explicit request. The black reference line is visible.

### 13.3 More contrasted map

| Constant | tonemap.live / before | Now | Effect |
|---|---|---|---|
| `spanMul` Relaxed / Medium / Hard | 4 / 4 / 8 | **2 / 2 / 4** | full color from ±18.5 / ±15.5 / ±10.1 c (before ±30.6 / ±25.8 / ±18.1 c) |
| `FULL_CONF_MS` (`kFullConfMs`) | 500 | **250** | full intensity after 0.25 s of accumulated sound |
| `STAIN_MIN_FRACTION` (`kStainMinFraction`) | 0.25 | **0.45** | a faded cell stays at 45 % of its base intensity |

The pure-center zone does not change (±6.17 c Relaxed, ±5.19 c Medium, ±2.02 c Hard) nor the needle/LED dead zone. With the new `FULL_CONF_MS`, `baseIntensity(125 ms)` = 0.5 and `baseIntensity(250 ms)` = 1; the bench examples (fade color, Relaxed EMA, 0.625 → 0.725, etc.) were rederived with the Python port.

### 13.4 Normal firmware without debug overlay
The whole overlay (`debugOverlay`), the `[tm]` serial line and the counters are under `#ifdef TONEMAP_DEBUG`, which only `cardputer-adv-tonemap-debug` defines. Verified that `cardputer-adv` (and `-debug`, `-led-debug`) binaries do not contain the strings `a/s%.1f`, `[tm] an/s` or `starved=`. The `[tonemap] enter/leave/paused` lifecycle notices over serial stay in all envs (not drawn on screen).

### 13.5 Options panel (Alt / Enter)
- **Input:** new `Key::Alt` in `input.h`; `input::poll()` emits it from `keysState().alt` with the same pattern as `enter`/`del` (one event per press, not repeatable). To avoid changing other modes (the metronome, for instance, cancels tap on any other key), `Mode::acceptsAlt()` (false by default) and `main.cpp` drop `Alt` except in the tuner with the ToneMap view (`TunerMode::acceptsAlt()` returns `tm_`).
- **View:** `menuOpen` (closed in `enter()`/`leave()`). `Alt` or `Enter` toggles. Open, `draw()` paints `drawMenu()` instead of the map and strip: black background, title `OPTIONS` + `Alt/Enter: close` on the right, and 13 lines (title + 12) at 9 px pitch (`S`, `H`, `C`, `N`, `K`, `F`, `P`, `L` with the value in the 13-character column after the label; `R`, `Space` + `Pause`/`Resume`, `Up/Down  A=<refA> (reference)`, `T Classic view`). Keys call the same handlers, so values update live; the transient message is not shown.
- **Cost:** closed, nothing changes. Open, analysis, fade polling and the debug period do not set `dirty`; only keys do. On close, `dirty = true` and the whole map is repainted (20 fps limit intact).
- **Strip hint:** `Alt:menu` (48 px, dim gray) left of the transposition/`A=…`, with 4 px margin each side. The right edge of the left block is computed with `textWidth`. Cases: `A=480` without transposition (right block from x=170): `Alt:menu` fits if the left block ends at x ≤ 114 (`440.0 Hz` ends at 118: shows `Alt`; `98.0 Hz`, `--`, `Below range`: full). With `Bb` (from x=152): full only if it ends at ≤ 96 (`--`, `Below range`, `Paused (Space)`); with note and 3951.0 Hz (ends at 124) shows `Alt` (x 130..148). 20-character messages (end at 122): `Alt`. `Microphone unavailable` (134) with transposition: nothing. Never touches the meter (x ≥ 206).
- **Not verified without the device:** that the keyboard matrix reports `Alt` pressed alone (on M5Cardputer it is a modifier; `keysState().alt` is set in `updateKeysState`, untested). `Enter` is the safe fallback.

### 13.6 Classic tuner with automatic detection (2026-10-22)
With ToneMap's engine, the classic view no longer searches only the chosen note (±2 semitones) but hears the whole range C2..B7 and shows the **nearest** note with the cents relative to it. The selector on top (`< 4A# >`, Left/Right) now chooses only the **drone** note (labeled `Drone <Hz>`).
- **Engine.** `tonemap_audio` (continuous capture at 32 kHz) -> `tonemap_detect::analyse` (the same code as the ToneMap view: `mpm::prepare` + rms gate + `mpm::findPitch` with hybrid K and maxLag 720 + clarity > 0.8 + 14 < Hz < 8200) -> `tonemap::freqToNote(hz, refA)` (transposition 0). Automatic gate since 2026-10-23 (§16; before, fixed rms >= 80, the "Low"). A note outside C2..B7 (36..107) is discarded. `tonemap_detect.cpp` holds the three analysis buffers (~15 KB) that used to be in `tonemap_view.cpp`; the ToneMap view calls the same function with no behavior change. `pitch.cpp/.h` (old ±2 semitone detector) was no longer used and was deleted (2026-10-24).
- **Needle and smoothing.** One reading per window (~31/s). Within the same note, exponential smoothing with tau = 185 ms (equals the 0.5 per 128 ms frame of the old detector); if the note changes or the jump exceeds 25 cents it is taken instantly. The reading is held 700 ms without valid windows and then the needle returns to rest (as before). The needle keeps its 70 ms exponential approach.
- **Entry and exit** (same discipline as `tonemap::enter/leave`, §7.5): enter = (`micStop` if the mic was on) `attach()` -> `audio::micStart()` -> `start()`; exit = `stop()` -> `audio::micStop()` -> `detach()`. The drone cuts it: enabling it does `stop/micStop/detach` then `droneStart`; disabling it does `droneStop` (with its fade) and re-enters. `leaveClassic()` does `droneStop` then exit. With the drone playing the dial goes gray and the center says `Mic off (drone playing)`.
- **Screen** (canvas 240 x 125): top left the drone selector (y 15) and `Drone <Hz>` (y 32); top center (x 118) cents (Font4, y 1) and Hz (Font0, y 28); top right `A=442 Hz` with arrows; at the dial center the detected note in DejaVu40 with baseline at y 121 and `drawSharp`'s `#` (30 px); the needle starts at r = 94 to leave the dial interior free. Footer: `Microphone unavailable` left, `Space: drone` right.
- **`#` sign** (`drawSharp`): two vertical 2 px bars (3 px from h = 24), the right one 2 px taller, and two same-width bars rising to the right and sticking out 2 px past the posts, drawn column by column with `fillRect`.

### 13.7 Extreme palette: continuous gradient and no dimming (2026-10-22)
The Extreme palette (formerly Classic) grew more extreme after device tests. First version: steep ramp (`t_e = |t| / 0.3`) and no dimming. Second: the user sees colors "jump", so the steep ramp and the pure green plateau are removed and **one continuous gradient over the whole cell range** remains. Color blindness is ignored (Safe covers it). Palette 1 only:
- **Ends:** pure sRGB primaries, low `(0,0,255)`, center `(0,255,0)`, high `(255,0,0)`; midpoints yellow and cyan (max luminance, no brown). ΔE76 (CIELAB, D65): green–blue 258.7, blue–red 176.3, green–red 170.6 (min 170.6). Among the cube vertices {R,G,B,C,M,Y} with the center green, no other (low, high) pair matches that minimum (next, magenta–red, gives 129.5).
- **Linear position** (`biasTLinear`, next to `biasT`): `t_lin = clamp(e' / (dz·(1 + spanMul)), −1, +1)` with `e' = avgErrRatio · errScale`; NaN gives 0. No dead zone: exact 0 is `(0,255,0)` and any deviation already changes the color. `paletteColor(1, t)` interpolates green→yellow→red (|t| 0, 0.5, 1) and green→cyan→blue. `cellColor` and `biasColor` use `biasTLinear` for palette 1 and `biasT` (with its dead zone) for Safe.
- **No dimming:** for palette 1 the effective intensity is `kExtremeMinDisplay + (1 - kExtremeMinDisplay) * display` = `0.75 + 0.25 * display`. A stained cell never drops under 75 %; faded ones (display 0.45) sit at 0.8625 and short notes (display 0.5) at 0.875. Visited but never stained stays black. Safe uses display as is.
- **Labels** follow the luminance rule over the shown color (§13.1).

| t_lin | RGB (display 1) | Relaxed (c) | Medium (c) | Hard (c) |
|---|---|---|---|---|
| −1 | (0, 0, 255) | −18.65 | −15.65 | −10.13 |
| −0.5 | (0, 255, 255) | −9.30 | −7.81 | −5.06 |
| −0.25 | (0, 255, 128) | −4.64 | −3.90 | −2.53 |
| −0.1 | (0, 255, 51) | −1.86 | −1.56 | −1.01 |
| 0 | (0, 255, 0) | 0 | 0 | 0 |
| +0.1 | (51, 255, 0) | +1.85 | +1.56 | +1.01 |
| +0.25 | (128, 255, 0) | +4.63 | +3.89 | +2.52 |
| +0.5 | (255, 255, 0) | +9.25 | +7.77 | +5.04 |
| +1 | (255, 0, 0) | +18.45 | +15.51 | +10.07 |

(Negative cents are slightly larger because the error is measured as a ratio, not in cents.)

Tests (`tonemap_bench.cpp`, values rederived in the Python port): exact palette points at `t_lin`, color cases with tolerance 2 (the history's 16-bit quantization weighs more on a gradient of ~65-100 levels per cent), blend at display 0.5, `biasTLinear` (0, NaN, ±inf, extremes), no plateau (+0.02 c already changes the red), fade (246,112,0 at 0.8625 and 220,100,0 at 0.45 at +12 c Medium) and the `0.75 + 0.25 * display` rule.

### 13.8 Live color: strip cents and LED (2026-10-22)
The player needs to see at a glance whether they are low or high and which way to correct, so the cents text (`+3.2c`) and the LED stop being "green / white" and show the color of the **instantaneous** deviation of the sounding note:
- **Pure function** `Rgb liveColor(int palette, Sensitivity s, float errRatio)` (`tonemap_model`): Safe → `paletteColor(0, biasT(s, errRatio))` (dead zone: exact center color); Extreme → `paletteColor(1, biasTLinear(s, errRatio))`. Full intensity: no 0.75 + 0.25·display, fade or blend with black. NaN gives the center color. Uses the frame's raw `errRatio` (`TickResult.errRatio`, the same as the dead zone), not Relaxed's smoothed needle nor the history average.
- **Strip:** cents are drawn in that color (stored in the `Snapshot` to redraw on change). Note and Hz as before.
- **LED** (`ledUpdate`): with `L` on, active note and not paused it shows the live color; off with no note, paused, `L` off and on exit (same paths as before, `ledOffBlocking` in `leave()`). The color is sent at note start and then only if some channel moved `kLedStep` = 16 levels or more and at least `kLedMinGapMs` = 50 ms passed since the last send; each send is repeated `kLedResends` = 2 times at 6 and 12 ms (the LED is only reliable at brightness 255, which the view already forces with LED on).
- **Examples** (Medium, color at error −20 / −10 / −3 / 0 / +3 / +10 / +20 cents): Safe (0,100,255) · (108,184,225) · (200,255,200) · (200,255,200) · (200,255,200) · (226,188,107) · (255,110,0); Extreme (0,0,255) · (0,184,255) · (0,255,98) · (0,255,0) · (98,255,0) · (255,182,0) · (255,0,0).
- **Risks:** Extreme's pure blue `(0,0,255)` is the darkest color on the black strip (contrast ≈ 2.4:1; Safe's blue `(0,100,255)` ≈ 4.3:1); in Extreme the color changes ~65-100 levels per cent, so the LED may flicker with strong vibrato (the 16-level threshold and 50 ms limit it); Safe's center `(200,255,200)` is almost white on the LED.
- Tests: `liveColor` (both palettes, dead zone, extremes, infinities, NaN, unknown palette, match with `cellColor` at display 1 in Safe) in the bench and the Python port.

### STOP C: measurement with real analysis and drawing (2026-10-23)
- Env `cardputer-adv-tonemap-debug`, viola, ToneMap view, 147 `[tm]` lines (~75 s), noise gate at `None` (analysis always on, worst case): `an/s` 31.2 mean (min 29.8; max 32.0), `an_ms` 9.6 (max 9.7), `draw_ms` 3.3, `push_ms` ~12.1, `fps` 15.6, `loop_ms` ~9, `starved=0`, `torn=0`, `skipped=3` constant (startup), `minheap=216504`. Brief §6 A criterion (≥ 25 analyses/s with the screen drawing) met.
- Finding: the app does not open the USB CDC port by itself; `Serial.println` does not reach the PC without `Serial.begin(115200)`. Added only in the `TONEMAP_DEBUG` build (`tonemap::enter()`); the normal app still does not call it (the `[mode]`, `[settings]`, `[tonemap]` logs only appear with that build or with a monitor that already opened the port).

## 14. LED shared with the classic view and battery icon (2026-10-23)

### 14.1 `tuner_led.h/.cpp`
The LED primitives that lived in `tonemap_view.cpp` moved, with no change of constants or behavior, to a small `tuner_led` module (namespace with its own state, no dependency on `tonemap_audio/model/detect`, so the bench env still builds):

| Function | What it does |
|---|---|
| `reset()` | forgets a previous run (`lit` = false, no repeats); does not touch brightness |
| `brightnessRaise()` / `brightnessRestore()` | saves the user's brightness and sets 255 / gives it back (no-op if not raised) |
| `show(r,g,b)` | sends `neopixelWrite` now and schedules 2 repeats (at 6 ms and 12 ms) |
| `service()` | every loop iteration: sends pending repeats |
| `offBlocking()` | 3 off commands 6 ms apart, before restoring brightness; acts only if the LED may be lit |
| `lit()` / `setLit()` / `red()` / `green()` / `blue()` | "may be lit" flag (kept by the caller) and last sent color |

The ToneMap view keeps its live color logic (`ledUpdate`: 16-level threshold and 50 ms) and only changes the calls. The classic view (`mode_tuner.cpp`) uses the same module: green `(0,255,0)` when `inTuneNow()` (reading shown, no drone, |smoothed cents| ≤ 5, the same condition that paints needle, note and cents green), off otherwise. The metronome keeps its own copy of the code (not refactored).

**A single owner of LED and brightness.** State lives in one place (`brightRaised`, `userBrightness`), and a view change always does `leave()` of one view (`offBlocking` + `brightnessRestore`) and then `enter()` of the other (`brightnessRaise` already reads the restored brightness): `TunerMode::onReselect` calls `tonemap::leave()` then `enterClassic()`, or `leaveClassic()` then `tonemap::enter()`. `TunerMode::leave` (mode change) goes through the same exit of the active view. On entering the metronome, it reads the already restored brightness. With `L` off no view raises brightness.

### 14.2 Battery reading (`battery.h/.cpp`, icon in `main.cpp`)
- **What M5Unified 0.2.25 gives** for the Cardputer and Cardputer ADV: both boards use the virtual PMIC `pmic_adc`: ADC1 on GPIO10 with ×2 divider (`_adc_ratio = 2.0`), read with `analogReadMilliVolts` (calibrated). `getBatteryVoltage()` returns mV (0 if the read fails) and `getBatteryLevel()` converts it linearly: `(mV − 3300) × 100 / 800`, clamped to 0..100 (≈ 3.3 V = 0 %, ≥ 4.1 V = 100 %); never returns < 0 with this PMIC. **`isCharging()` returns `charge_unknown`** for these boards (no branch for them: no readable charge chip), so the charging indicator never activates; the branch is ready in case a future M5Unified implements it.
- **Sampling:** every 5 s, median of 5 consecutive `getBatteryLevel()` reads (removes ADC noise). If `getBatteryVoltage()` is outside 2500..4700 mV (0 = failed read, or floating pin with no battery) or the level is < 0, the reading is −1.
- **Smoothing:** median of the last 3 readings (−1 included) → segments; the icon changes only when the new segment count repeats in 2 consecutive readings (temporal hysteresis: a change takes 10-15 s, reasonable for a battery). The first reading is shown instantly (`battery::begin()` in `setup()`).
- **Segments:** `ceil(level / 20)`, minimum 1 with level > 0, 0 at level 0, −1 = unavailable (21..40 % = 2 yellow segments, 1..20 % = 1 red segment, 0 % = empty red outline).
- **Drawing** (`drawBattery` in `main.cpp`, bar rows y+2..y+8, x0 = 226): 12 × 7 outline + 1 × 3 nub (13 px), five 2 × 5 px segments inside the outline, no gaps. Colors: normal = `kVolColor` (the `Vol` text's, cyan), 2 segments = yellow, 1 or 0 = red, unavailable = `DARKGREY` outline with no segments, charging (if ever reported) = white outline. The `Vol` text aligns to the right of the icon with a 3 px gap (`Vol 10` loses the space if it does not fit next to the last mode).
- **Redraw:** `loop()` calls `battery::tick(millis())`; if the segment count (or charge) changes, `drawBar()` is called. Modes do not intervene (the bar belongs to `main.cpp`).
- **Debug:** only with `TUNER_DEBUG` or `TONEMAP_DEBUG`, one serial line every 5 s: `[battery] raw=<level> mv=<mV> charging=<0 no|1 yes|2 unknown> hist=a,b,c cand=<segments> segs=<shown segments>` (with `Serial.begin(115200)` in `battery::begin()`, only in those envs). The normal env has no log or overlay.

## 15. Octave continuity guard: tried and removed (2026-10-24)

**Historical record: the guard NO LONGER exists in the firmware.** Implemented 2026-10-23 (`tonemap_detect::OctaveGuard`) and removed 2026-10-24 after testing on the device.

### 15.1 What it was
The hybrid K (§12) still produces short octave excursions with low viola notes (F3/E3/D3 read as F4/E4/D4 for 1-6 frames; some A3 → A2 from the K = 0.97 re-pick). The guard compared each frame with the previous one (in raw Hz): if it was exactly an octave away (±0.04 octave), it returned it folded to the previous octave and accepted it only if it repeated in 4 consecutive frames (~128 ms); an invalid frame reset it. Applied before `model.tick` (ToneMap) and `freqToNote` (classic view).

### 15.2 Simulation numbers
Reproducing the C++ detector in Python over the long viola recording, with N = 4 octave jumps went from 33 to 16.

### 15.3 Why it failed on the device
Sergio noticed more upper-octave false positives than before. The wrong frames are mostly in the note's **attack** (onset harmonics): if the FIRST frame is F4, the guard takes that frame as reference and folds upward the correct F3s of the next 3-4. Repeated on a sustained F3: before, the note started with 1 wrong frame; with the guard, with 4, and it repeated after each short excursion. The guard favors the octave heard first, which at attacks is the wrong one. Removed entirely (structure, constants, calls, resets and the bench's `[test] octave guard` series); back to using `an.hz` directly in both views.

## 16. Mute: Min gate level and automatic gate in the classic view (2026-10-23)

### 16.1 Measurements
Recording playing viola with a **practice mute** (mic at 50-60 cm): silence rms 5-6; playing, rms median 75, p25 48, p5 27. Gate 80 ("Low") loses 55 % of detectable frames; with gate 30, 93 % are kept and there are no ghosts (silence is rejected anyway by clarity > 0.8).

### 16.2 ToneMap view: 5-level manual gate
`kGateRms = {0, 30, 80, 200, 450}` named `None`, `Min`, `Low`, `Med`, `High` (`tonemap_detect.h`, with `kGateNames`, `kGateLevels = 5` and `kGateDefault = 2`). `Low` (80) keeps its meaning and stays the default. Key `N` rotates None → Min → Low → Med → High → None (message `Noise gate: Min`), and the options panel, the level meter mark and the `gate=` log use the same table. `tmGate` now has range 0..4 (`clampAll`).

**Migration (NVS).** NVS keys cannot silently change meaning: `tmGate` stores the old scale (0 none, 1 low, 2 med, 3 high). The new scale uses key `tmGateB` (7 chars, ≤ 15). `settings::load()`:
1. if `tmGateB` exists, it is read and used;
2. if not but `tmGate` does, `tmGate = migrateGate(oldValue)` with {0,1,2,3} → {0,2,3,4} (any other value → 2) and a serial line `noise gate migrated to the new scale` is written;
3. if neither exists, the default stays (2, Low).

`settings::tick()` writes only `tmGateB`; the old key is left untouched and ignored when `tmGateB` exists. With no user changes it migrates again on every boot, with the same result.

### 16.3 Classic view: automatic gate (no key, no setting)
`tonemap_detect::AutoGate` (pure struct, tested in the bench):
- `floor` (noise floor, rms units) is initialized with the first window after `reset()` (`max(rms, 1)`).
- Each window: if `rms < floor`, `floor = max(rms, 1)` instantly; else `floor += (rms − floor) × min(1, dtMs / 30000)` (slow upward drift, 30 s time constant; `dtMs = 0` is taken as 32).
- Gate = `clamp(2.5 × floor, 20, 80)`. Silent room (floor ~6) → 20; floor 16 → 40; floor ≥ 32 → 80. Never above the old 80, so behavior in a noisy room does not get worse.
- A NaN or negative rms carries no information: it does not move the floor. rms is capped at 32768.
- `tonemap_detect::analyse(AutoGate&, dtMs)`: `mpm::prepare` runs on **all** windows, the floor updates with their rms (also if under the gate, so it can fall in silence) and only then is compared with the returned threshold; `findPitch` only runs above. `Analysis::gate` carries the threshold used (the `[tuner]` log shows it). The ToneMap view keeps calling `analyse(float gateRms)` unchanged.
- Reset: together with the capture (`startCapture`/`stopCapture`: enter, exit, drone on/off), not on every lost reading. If entering while playing, the floor starts high (up to 80) and drops as soon as a quieter window arrives.
- The classic view no longer uses `tmGate` (nor has key `N`).

### 16.4 Risks
With the gate at 20, ambient noise enters the detector more easily (held back by clarity > 0.8); continuous tonal noise (mains hum) raises the floor and the gate, which is the intended behavior. If the player holds a very loud long note right on entering, the floor takes until the next pause to drop. Not verified on the device.
