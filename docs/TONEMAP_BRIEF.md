# Implementation brief — ToneMap view in the tuner

Date: 2026-10-16. Decided with Sergio in conversation (three rounds of questions).
Ported reference: [crossrhythm/tonemap.live](https://github.com/crossrhythm/tonemap.live), commit `bd19db1`, MIT © 2025 Jeremy Black. Reference detector: `pitchy@4.0.1` (npm), MIT.

For **Claude Code on Sergio's computer**. Contains the closed spec (§2), the extracted ToneMap logic (§3), the ESP32-S3 adaptation (§4), the phase plan with agent per phase and device-test stops (§5), and acceptance criteria (§6).

---

## 0. How to run this brief (instructions for Claude Code)

1. Read `CLAUDE.md` and `docs/SPEC.md` first. Their rules win over this brief on conflict (above all: **no USB flashing without permission**, commit + push after every successful build, all firmware text in English, silent interface).
2. Work on branch `feature/tonemap` (already on GitHub, contains this brief: `git fetch origin && git checkout feature/tonemap`). **Do not merge into `main` unless Sergio says so.**
3. Run the phases of §5 in order. The main session is the **orchestrator**: launches agents, reviews results, builds, commits.
4. At each **STOP**: build, leave `firmware.bin` ready, tell Sergio exactly what to test and what to watch on serial, wait for the answer. If Sergio asks to flash and the Cardputer is connected, flash per `CLAUDE.md`.
5. **Scope:** touch only what the ToneMap view needs: `mode_tuner.cpp`, new files, `settings.*`, one line of `main.cpp` and an empty method in `ui.h` (§4.7). Metronome, game, About, `pitch.*` and `audio.*` stay untouched (unless phase 2 proves it essential; then ask).
6. If anything in this brief proves impossible or ambiguous in the real code, **ask Sergio** instead of changing behavior on your own. Internal technical decisions (structures, names) are yours.

---

## 1. What is built

A second view inside the tuner (`T`), inspired by tonemap.live: a grid of 12 columns (notes) × 6 rows (octaves C2–B7) where each note you play tints its cell by your accumulated tuning **tendency** (orange = tend high, green = centered, blue = tend low). The sounding cell gets a white border and a cents needle. It is an intonation heat map for practice, not a target-note tuner.

Sergio's main use: **strings / high winds** (violin, viola, flute, clarinet…).

---

## 2. Functional spec (copied to `docs/SPEC.md` in phase 0)

The full current spec, with later changes, is in `SPEC.md` ("Tuner — ToneMap view"). Original decisions:

### 2.1 Entering and leaving
- Inside the tuner **`T` toggles** between classic view (needle + target note) and **ToneMap**. Arriving from another mode opens **the last view used** (NVS). Bottom bar unchanged. Entering ToneMap with the drone playing stops the drone and starts the mic (exclusive on the ADV). **No drone** in ToneMap.

### 2.2 Screen layout (content canvas 240 × 125)
Info strip on top (Font0, 6×8 px chars, height 15 px + 1 px dark gray line), then the grid from y=17: 6 rows × 18 px (y 17..124), 12 columns × 20 px; top row = highest octave, C2 bottom-left, B7 top-right. Fixed range in v1 (written MIDI 36..107). Diagram: see `SPEC.md`.

### 2.3 Cells
- Grid lines 1 px `#374151`. Background `#000000`.
- **Label:** note name only (`C#`, or `Db` with flats); the C column also carries the octave (`C4`). Font0, color `#4b5563` while never touched, white once touched (`visited`).
- **Background color** = blend of black and the trend color by intensity (§3.4). Fixed palette (ToneMap's): high `#FFAA00`, center `rgb(34,197,94)`, low `#0056D6`. *(Replaced 2026-10-22: two palettes, Safe and Extreme (formerly Classic), key `P`; see §9 and `SPEC.md`.)*
- **Active cell** (note sounding now, after the gates of §3.2): inner white 2 px border *(replaced 2026-10-22 by four L corners and then, the same day, by a white bottom bar + white label; see `SPEC.md`)*, a black 2 px reference line at the cell's vertical center and the **needle**: red bar `#ef4444`, 2 px high across the cell. Up = high. Travel 10 %–90 % of height for −50…+50 cents (formula §3.5).

### 2.4 Info strip (top row)
- Left: detected note and written octave (`A4`), signed cents with one decimal (`+3.2c`), real Hz. Cents green inside the mode's dead zone (§3.3), white otherwise. No valid note: `--`.
- Out of range (written note < C2 or > B7): `Below range` / `Above range`.
- Right: transposition if not C (`Bb`, `Eb`, `F`), `A=442` (same A as the classic tuner), mic level meter (~30 px) with a mark at the noise gate threshold.
- **Transient messages** on setting changes, 1.5 s, in the place of note/cents/Hz: `Sensitivity: Hard`, `History: 30 s`, `Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`, `LED: off`, `Map cleared`. Paused: fixed `Paused (Space)`.

### 2.5 Keys in the ToneMap view

| Key | Action |
|---|---|
| `T` | Back to classic view |
| Up / Down | Reference A ±1 Hz (same `refA` as classic, 400–480, persistent) |
| `S` | Sensitivity: Relaxed → Medium → Hard → … |
| `H` | History: 1 s → 3 s → 30 s → Unlimited → … |
| `C` | Color hold: 5 s → 15 s → 30 s → 2 min → 5 min → No fade → … |
| `N` | Noise gate: None → Low → Med → High → … |
| `K` | Transposition: C → Bb → Eb → F → … |
| `F` | Sharps ↔ flats in labels and strip |
| `L` | Tuning LED on/off |
| `P` | Palette Safe ↔ Extreme *(added 2026-10-22; formerly "Classic")* |
| `R` | Clear the map (all cells) |
| `Space` | Pause / resume listening (map freezes; nothing painted while paused) |
| Left / Right | Unused in v1 (reserved for future temperaments) |

Global keys (`M`, `G`, `A`, `-`, `=`) are still captured by `main.cpp` before the mode: do not use those letters.

### 2.6 Settings, defaults, persistence
All saved in NVS with the existing deferred mechanism (`settings::touch()`).

| Setting | Values | Default | NVS key (≤15 chars) |
|---|---|---|---|
| Tuner view | classic / tonemap | classic | `tunerView` |
| Sensitivity | relaxed / medium / hard | medium | `tmSens` |
| History | 1 s / 3 s / 30 s / unlimited | 3 s | `tmHist` |
| Color hold | 5 s / 15 s / 30 s / 120 s / 300 s / no fade | 30 s | `tmHold` |
| Noise gate | none / low / med / high | low | `tmGate` |
| Transposition | C (0) / Bb (−2) / Eb (+3) / F (+5) | C | `tmTransp` |
| Accidentals | sharps / flats | sharps | `tmFlats` |
| Tuning LED | on / off | on | `tmLed` |
| Palette *(added 2026-10-22)* | safe / extreme | safe | `tmPal` |
| Reference A | 400–480 Hz | 442 (exists) | `refA` (exists) |

Transposition values are ToneMap's (semitones; written note = concert note − value, see §3.2). Adding A (clarinet in A, −3) is trivial on request.

### 2.7 The heat map
- Behavior = ToneMap (§3). The map lives in RAM **while the device is on**: survives mode/view switches; cleared with `R` or power off. Not saved to flash or SD.
- Changing A **re-scores** the history (colors recomputed against the new reference), as in ToneMap.
- Changing transposition **shifts** the map (same concert note lands in another written cell), as in ToneMap. Whatever leaves the stored range is lost.
- Changing sensitivity or History recomputes all cell colors from the stored history.

### 2.8 LED
- With `LED` on: RGB LED (GPIO 21, `neopixelWrite`) lights **green** while the active note is inside the mode's dead zone and turns off when it leaves, there is no note, or paused.
- Known hardware restriction (see SPEC, metronome): LED shares the backlight pin and **only works at screen brightness 255**. With LED on, the ToneMap view saves the user's brightness, sets 255 and restores it on leaving the view or mode, or on LED off. Repeat each LED command twice (at 6 and 12 ms), like the metronome.
- Do not refactor the metronome's LED code; duplicate the minimum in ToneMap code.

### 2.9 Out of scope in v1
Temperaments and tonal center, Railsback stretch, Note View (Ribbons/Balloons), recorder and WAV, palette editor, AGC, statistics, saving the map to SD, changing the visible range. The architecture must not prevent adding temperaments later (variable-width columns): frequency → note conversion must go through a single "target offset per note" function, as in ToneMap.

---

## 3. Technical reference extracted from ToneMap

All in tonemap.live `index.html` (inline JS, ~9000 lines). Line numbers change: **search by function name**. Phase 1 verifies this summary against the original.

### 3.1 Detector (pitchy 4.0.1, `PitchDetector.findPitch`)
McLeod Pitch Method over a 2048-sample window (Web Audio, 44.1/48 kHz, `fftSize 2048`, `smoothingTimeConstant 0`), run on every `requestAnimationFrame` (~60 Hz, overlapping windows).
1. Autocorrelation r'(τ) for τ = 0..N−1 (pitchy uses FFT; same result as direct sum).
2. Type II NSDF: `n(τ) = 2·r'(τ) / m'(τ)`, with `m'(0) = 2·r'(0)` and `m'(τ+1) = m'(τ) − x[τ]² − x[N−1−τ]²`. If m' ≤ 0, the rest of the NSDF is 0.
3. **Key maxima**: walk the NSDF; after an ascending zero crossing look for the maximum until the next descending crossing; that maximum is a key maximum.
4. `nMax` = largest key maximum. Result = **the first key maximum ≥ 0.9 · nMax** (K = 0.9).
5. Parabolic refinement with both neighbors; `freq = sampleRate / refined τ`, `clarity = min(1, parabola value at the maximum)`.

Metrotonopitecus's classic tuner (`pitch.cpp`) already computes the same NSDF but only within ±2 semitones of the target note and picks the largest peak. ToneMap needs the full range and K = 0.9 selection (this avoids octave errors).

### 3.2 Gates and note conversion (`tick()`, `freqToNoteData()`)
- Valid frame if `clarity > 0.8`, `14 < freq < 8200` and `rms ≥ noise threshold` (`loudEnough`).
- Transposition: `adjustedPitch = pitch · 2^(−transposition/12)`. With Bb (−2) the written note is 2 semitones above concert. Everything else (cells, labels) works in **written notes**.
- Equal temperament without stretch: `midi = round(12·log2(f/A4)) + 69`, `targetFreq = A4 · 2^((midi−69)/12)`, `cents = 1200·log2(f/targetFreq)`, `errRatio = (f − targetFreq)/targetFreq`. ToneMap builds the target as `440 · 2^((midi−69)/12) · 2^(targetOffsetCents/1200)` with `targetOffsetCents = latticeOffset(midi) + 1200·log2(A4/440)` (v1: `latticeOffset = 0`). Algebraically the same; **keep this form** (a `targetOffsetCents(midi)` function) to add temperaments later and for the re-scoring of §3.4.
- `onset filter` (attackMs) defaults to 0 in ToneMap: the note is accepted on the first frame. No onset filter in v1.

### 3.3 Sensitivity modes (constants)

| Mode | `DEADZONE_RATIO` | `ERR_SCALE` | `NEEDLE_TAU_MS` | Dead zone (needle/LED) | Pure center color up to | Saturated color from |
|---|---|---|---|---|---|---|
| Relaxed | 0.0025 | 0.7 | 350 | ±4.3 c | ±6.2 c | ±31 c (span = 4·dz) |
| Medium | 0.0021 | 0.7 | 80 | ±3.6 c | ±5.2 c | ±26 c (span = 4·dz) |
| Hard | 0.00105 | 0.9 | 30 | ±1.8 c | ±2.0 c | ±18 c (span = **8**·dz) |

- **Dead zone** (green cents text, LED): `|errRatio| ≤ DEADZONE_RATIO` (unscaled).
- **Trend color** (`biasRelaxed/biasMedium/biasHard`): `e = avgErrRatio · ERR_SCALE`; if `|e| ≤ dz` → center color; else `t = clamp((|e| − dz) / span, 0, 1)` and interpolate linearly center → high (e > 0) or center → low (e < 0). `span = 4·dz` in Relaxed and Medium, `8·dz` in Hard.
- **Needle**: displayed cents are smoothed with an EMA `alpha = 1 − exp(−dt/τ)` with the mode's τ. EMA resets on note change or silence.

### 3.4 History, intensity and fade (`finalizeActiveSegment`, `getRecentWeightedErr`, `updateCellDisplayColor`)
Per-cell state (`createEmptyCellState`): `totalVoiceMs`, `segments[]`, `biasRGB`, `baseIntensity`, `displayIntensity`, `lastStainAtMs`, `fadeAnchorIntensity`, `visited`.
- With attackMs = 0, **each valid frame produces a segment** `{startMs, durMs = dt, avgErrRatio = errRatio, targetOffsetCents}` in its note's cell and re-tints the cell in that same frame.
- `totalVoiceMs += durMs` (since last reset, no window). `baseIntensity = min(1, totalVoiceMs / 500)` (`FULL_CONF_MS = 500`).
- **Color**: **duration-weighted** mean of `errRatio` over the last `History` seconds **of sound** (walk segments backwards summing durations until the window is covered; silence does not count). Each segment is re-based before averaging: `rebase(err, from, to) = 2^((from − to)/1200) · (1 + err) − 1`, `from` = its `targetOffsetCents`, `to` = the note's current one. So changing A re-scores the history. Unlimited = all segments. The result goes through the mode's `bias*` (§3.3) → `biasRGB`.
- **Display** every frame (`updateCellDisplayColor`, `STAIN_MIN_FRACTION = 0.25`, `STAIN_FADE_SECONDS = 10`, `STAIN_FADE_IN_MS = 125`):
  - Medium and Hard: `display = baseIntensity`.
  - Relaxed: while staining, `display` approaches `baseIntensity` with a 125 ms EMA; otherwise it holds.
  - Fade: nothing until `holdSeconds` after the last stain. Then a linear ramp over 10 s from its value down to `0.25 · baseIntensity`, and it stays there (never reaches 0 until a reset). No fade = no ramp.
  - `colorFinal = lerp(background, biasRGB, display)` per channel (black background).
- Changing sensitivity or History → `recomputeAllCellColorsAndRender()`: recomputes `biasRGB` and intensity of all cells from their segments.
- Changing transposition → `shiftCellStateBySemitones(old − new)`: each note's state moves to another cell.
- ToneMap keeps up to 2000 segments per note for `max(History, ColorHold) + 5 s` (max 10 min). **Do not copy that** on the ESP32: see §4.4.

### 3.5 Active cell and needle (`highlightGridCell`, `getNeedleCellPosition`)
- Equal temperament: `pct = (clamp(cents, −50, 50) + 50) / 100` (0 = low edge, 1 = high edge).
- Horizontal needle (ToneMap default): `top = (90 − pct·80) %` of cell height. Black reference line at `targetPct = 0.5` → 50 %.
- The active cell is marked `visited`.

### 3.6 Corrections verified against the source (phase 1, 2026-10-16)
Phase 1 compared §3 with `tonemap.live/index.html` (bd19db1) and `pitchy@4.0.1`. Constants, color formulas, re-basing, fade, needle and the MPM algorithm match. What follows **corrects or completes** the above and takes precedence. `index.html` line numbers in parentheses.

**Corrections**
- **Needle (§3.3, last point):** with `attackMs = 0` the needle EMA only acts in **Relaxed** (12083-12135: in Medium and Hard `relaxedSmoothingActive` is set to false every frame and the next one is re-seeded with raw cents). **Decision: replicate ToneMap as is**: Medium and Hard show the frame's raw cents; Relaxed uses EMA with τ = 350 ms. Reset on note change or an invalid frame.
- **Segments (§3.4, first point):** the **first valid frame of each note** (after silence or note change) **produces no segment** (12130-12132, 10500). A frame's segment carries `dtMs` = time since the previous tick, uncapped; if `dtMs ≤ 0` no segment. The segment's `avgErrRatio` is the frame's raw `errRatio`.
- **Dead zone (§3.3, §2.8):** the rule `|errRatio| ≤ DEADZONE_RATIO` (raw, per frame) is right, but in ToneMap it only highlights a debug text. The green cents text and the LED are Metrotonopitecus design.
- **Retention (§3.4):** ToneMap prunes by wall-clock time (`max(History, Hold)·1000 + 5000` ms, cap 10 min) and at 2000 segments per note (≈33 s of sound at 60 seg/s). Do not copy (see §4.4).

**Omissions**
- **Gates:** valid frame = `hasSignalEnergy && clarity > 0.8 && 14 < pitch < 8200`, with `hasSignalEnergy = rms > 1e-7 || peakAbs > 4e-6`; the Hz bounds apply to the **untransposed** pitch. ToneMap's noise gate is RMS on a ±1.0 scale: `[0, 0.025, 0.05, 0.10, 0.25, 0.50]` (None/Min/Low/Med/High/Max, default 0.025). The int16 values of §4.3 are the Cardputer adaptation, to calibrate at STOP A.
- **A single invalid frame ends the note** (12157-12164): the segment is closed, the active note cleared and the needle removed. No hysteresis. This resets the EMA and makes the next valid frame produce no segment.
- **Pitchy:** `findPitch` returns `[0, 0]` with no key maxima. The key-maxima loop runs i = 1..N−2; `NSDF[0] = 1` never counts as an ascending crossing; the maximum is seeded with the crossing sample and only recorded at the next descending crossing (an unclosed final lobe is discarded); crossings with `≤ 0` and `> 0`. The NSDF covers τ = 0..N−1 with no upper limit (pitchy uses Float32 FFT: in the tail of large τ, where `m'` is small, the direct sum may differ slightly; not a bug).
- **`freqToNoteData`:** starts from `round(semitones from A4) + 69` and tests ±2 candidates (0..127) minimizing `|(pos − cand)·100 − offset(cand)|`. In equal temperament without stretch it equals `round()`, except on the exact .5 tie (ToneMap picks the lower, `Math.round` the upper). Irrelevant in practice. `getTargetOffsetCents = getLatticeOffsetCents + getA4OffsetCents`, with `getA4OffsetCents = 1200·log2(A4/440)`. With stretch in the future, this is indexed by the concert note and the temperament by the written one.
- **Color (`updateCellDisplayColor`):** blends from `--grid-bg` (black in dark theme) with rounded channels; a cell is painted only if `displayIntensity > 0.001`. If `baseIntensity ≤ 0` or the cell was never stained (`lastStainAtMs = 0`), display = 0. In Relaxed the approach `alpha = 1 − exp(−Δms/125)` uses the time since that cell's last update and applies only on the frame it was stained.
- **`getRecentWeightedErr`:** the segment at the window edge enters partially (`take = min(durMs, remaining)`); returns 0 if total duration is 0.
- **`recomputeAllCellColorsAndRender`** (on changing History, sensitivity, A or transposition) sets `lastStainAtMs = now` and `fadeAnchorIntensity = null` in **all** cells: the hold/fade timer restarts. In Relaxed it does not pull intensity toward the base (only Medium/Hard jump). Changing only Color hold re-renders without resetting anything.
- **Transposition:** ToneMap's order = close segment, shift state by `old − new`, clear the active note, remove the needle. The shift discards what falls outside MIDI 0..127; stored `targetOffsetCents` are not rewritten (re-based on recompute).
- **Needle:** the painted cents are the smoothed ones (`displayCents`); `highlightGridCell` is called twice per tick.
- **Fixture (phase 4):** `tests/fixtures/temperament-vectors.json` is an object with `rows[]` (5192), not an array. The 264 `equal`/`none` rows are only **44 distinct vectors** repeated 6 times (`pitchCenter`/`anchor` do not matter): midi 33, 45, 57, 60, 64, 67, 69, 72, 81, 93, 105 × cents {0, 30} × A4 {440, 441}, `transposition` = 0. They do not cover negative cents, clipping at ±50 or rounding at the boundary. **Phase 4 must add own cases** (negative cents, ±50, ±49.9, rounding tie, transposition, MIDI extremes) besides the 264 rows. `expect.needleEven = [pct, targetPct]`; the brief uses `[0]`.

---

## 4. ESP32-S3 adaptation (Cardputer ADV)

### 4.1 Continuous capture with overlap
- The classic records 2048 samples, processes, records again: gaps, ~8 readings/s. ToneMap needs **continuous** capture and **25–30 readings/s**.
- M5Unified 0.2.25 `Mic_Class` allows **two queued recordings** (`_rec_info[2]`; `isRecording()` returns 0, 1 or 2; `record()` fails if the queue is full). Scheme: chunks of `HOP` samples in different buffers, two always queued; when one finishes it is copied into a ring of `WINDOW` samples and the next is queued. The **most recent window** is always analyzed; if analysis lags, windows are skipped (analyses are never queued).
- Starting point: `WINDOW = 2048`, `HOP = WINDOW / 2`. Phase 2 confirms by reading the M5Unified code.
- **Sample rate: 16 kHz or 32 kHz, decided by phase 3 by measuring.** At 16 kHz, B7 (3951 Hz) has a period of ~4 samples and octave-7 precision will be poor. At 32 kHz B7's period is ~8 samples and C2 (65.4 Hz) ≈ 489 lag, which fits `WINDOW = 2048`. 32 kHz is the initial bet given Sergio's high register. Measure cents error at C2, C4, C6, C7 and B7 and time per analysis. If octave 7 does not reach ±3 c even at 32 kHz, accept and document as "best effort".

### 4.2 Full-range detector
- New file, no Arduino dependencies (like `pitch.cpp`): implementation of §3.1 in `float` (S3 FPU is single precision; no `double`).
- Direct sum (O(N·τmax)) with pitchy's incremental denominator. Limit τ to `[sampleRate/4200, τmax]` with τmax for ~62 Hz. Estimate: 10–20 ms per analysis; **measure it**.
- If too slow, options in order: (a) lower `WINDOW` to 1536, (b) decimate to 16 kHz for large τ, (c) FFT autocorrelation with `esp-dsp`. (c) is **a new dependency: ask Sergio first**.
- Do not touch `pitch.cpp`: the classic tuner keeps its detector. (Note 2026-10-24: the classic ended up using ToneMap's detector and `pitch.cpp/.h` was deleted.)

### 4.3 Mic thresholds
- Cardputer amplitudes are not a browser's. The classic uses `kMinRms = 120` (int16) and `kMinClarity = 0.75`.
- ToneMap uses `clarity > 0.8`. Start with 0.8 and tune at STOP A.
- Noise gate (RMS in int16 units, to calibrate at STOP A): `None = 0` (clarity gate only), `Low = 120` (the classic's, proven), `Med ≈ 300`, `High ≈ 800`.

### 4.4 History memory (do not replicate 2000 segments per note)
- The weighted mean and re-basing are **affine** in `err`, so **consecutive frames can be merged** into chunks without changing the result (except at the window edge, with error ≤ one chunk's duration).
- Proposal (phase 2 closes it): per note keep `totalVoiceMs`, the running sum for Unlimited, a **fine** ring (chunks ≤ 100 ms, covers ≥ 3.2 s for History 1 s and 3 s) and a **coarse** ring (chunks ≤ 1 s, covers ≥ 30 s). Each chunk holds `durMs` and `err` (int16 in hundredths of a cent is enough). On changing A, re-base all stored chunks in place (equivalent to ToneMap's per-segment re-basing, since the transform is affine).
- Stored range: at least the 72 visible notes; ideally MIDI 0..127 if it fits, so transposition loses no data.
- **Budget: ≤ 40 KB for the whole map state.** Check in phase 2 whether the ADV has PSRAM (`ESP.getPsramSize()` over serial). Assume none.
- Map state must live outside the view object (static or the mode's) to survive mode switches.

### 4.5 Drawing
- Existing 240×125 16-bit canvas; `draw()` limited to ~30 ms per frame, like the classic.
- Redrawing the whole frame is acceptable: 72 rectangles plus text. Colors are blended in RGB888 and converted with `color565`.
- Measure `draw()` + `pushSprite` + analysis time in the bench. If the loop does not reach ~25 readings/s with the screen at 30 fps, drop to 20 fps.

### 4.6 Time
- Use `millis()` in the model passed as a parameter, so the model is pure and testable.
- Each segment's `dt` = real duration of the jump between analyzed windows (`HOP / sampleRate` if none skipped).

### 4.7 Changes outside the new files (minimal)
- `ui.h`: add an empty `virtual void onReselect() {}` to `Mode`.
- `main.cpp`: in `handleKey`, if the key is the current mode's hotkey, call `current->onReselect()` instead of doing nothing. Other modes keep behavior (their `onReselect` is empty). Do not send the key to `onKey`: the metronome would cancel its tap measurement.
- `settings.h/.cpp`: the fields and keys of §2.6 (with `clampAll`, `load`, `tick`).
- `mode_tuner.cpp`: current view (classic or ToneMap) and `onReselect()` that toggles. In ToneMap it delegates `enter`, `leave`, `onKey`, `update`, `draw` and `afterPush` to the new view. The classic part is unchanged.
- `platformio.ini`: bench and debug environments (§5). No new libraries.

### 4.8 Suggested new files (phase 2 may adjust)
- `src/pitch_mpm.{h,cpp}`: full-range detector (no Arduino).
- `src/tonemap_model.{h,cpp}`: notes, segments and chunks, colors, fade, re-basing, transposition shift, reset (no Arduino; takes `nowMs`).
- `src/tonemap_audio.{h,cpp}`: continuous overlapped capture over `M5Cardputer.Mic`.
- `src/tonemap_view.{h,cpp}`: drawing, keys, LED and strip; used by `mode_tuner.cpp`.
- `src/tonemap_bench.cpp`: only with `-DTONEMAP_BENCH` (§5, phase 3).
- Header on ported files: `Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)`.

---

## 5. Execution plan by phases and agents

Claude Code agents: `Explore` (read-only, fast code sweeps), `Plan` (read-only architect, returns a plan), `general-purpose` (reads, writes, runs). Read-only agents return text and the orchestrator writes it into `docs/`.

| Phase | Agent | Deliverable | Stop |
|---|---|---|---|
| 0 Preparation | orchestrator | branch, updated SPEC, references at hand | — |
| 1 Verify the reference | `Explore` (very thorough) | list of discrepancies with §3 (or "OK") | — |
| 2 ESP32 design | `Plan` | `docs/TONEMAP_DESIGN.md` | only if it contradicts §2 |
| 3 Capture + detector + bench | `general-purpose` | `pitch_mpm`, `tonemap_audio`, bench env | — |
| 4 Map model + bench | `general-purpose` | `tonemap_model` + parity tests | **STOP A** |
| 5 View and integration | `general-purpose` | firmware with the ToneMap view | **STOP B** |
| 6 Independent review | new `general-purpose`, no context, no edits | findings report | — |
| 7 Fixes and fine tuning | orchestrator (+ `general-purpose` if needed) | final firmware, SPEC with checkboxes | **STOP C** |

- **Phase 0 (orchestrator):** `git fetch origin && git checkout feature/tonemap` (rebase on `origin/main` if it advanced since 2026-10-16). Add to `docs/SPEC.md` a section "Tuner — ToneMap view" with §2 and link this brief. References **outside the repo** (e.g. `..\refs\`): `git clone --depth 1 https://github.com/crossrhythm/tonemap.live`; `npm pack pitchy@4.0.1` and unpack it (or https://unpkg.com/pitchy@4.0.1/index.js); M5Unified 0.2.25 sources are in `.pio/libdeps/cardputer-adv/M5Unified` after a `pio run`. Commit `docs: ToneMap section in SPEC`, push.
- **Phase 1 (`Explore`):** read-only; compare §3 against `tonemap.live/index.html` (functions `tick`, `freqToNoteData`, `getTargetOffsetCents`, `biasRelaxed/Medium/Hard`, `finalizeActiveSegment`, `getRecentWeightedErr`, `rebaseErrRatio`, `updateCellDisplayColor`, `recomputeAllCellColorsAndRender`, `shiftCellStateBySemitones`, `highlightGridCell`, `getNeedleCellPosition`, mode/stain/history constants) and `pitchy/index.js` (`findPitch`, `_nsdf`, `getKeyMaximumIndices`, `refineResultIndex`). Report every wrong or incomplete formula, constant or behavior, quoting the source; say so if all matches. The orchestrator fixes §3 and commits.
- **Phase 2 (`Plan`):** design the ESP32-S3 implementation following §2–4 and the current code; read M5Unified `Mic_Class` to confirm the two-slot queue and pick the capture scheme. Deliver: files with responsibilities and public interfaces; map data structures with byte counts (≤ 40 KB, no PSRAM); capture/analysis/draw schedule inside the existing `loop()` with CPU estimates; exact `onReselect`, settings and `mode_tuner.cpp` changes; bench and debug environments and what they print. Classic tuner, metronome, game, About untouched. Flag contradictions instead of changing the spec. Saved to `docs/TONEMAP_DESIGN.md`; stop to ask Sergio only if the design needs a change to §2 or a new dependency.
- **Phase 3:** `pitch_mpm` (§3.1/§4.2) and `tonemap_audio` (§4.1). Env `cardputer-adv-tonemap-bench` (`-DTONEMAP_BENCH`) replaces the app at boot: **synthetic precision** (sines and sawtooths with harmonics at 16 and 32 kHz for C2, A2, C3, C4, A4, C5, C6, C7, A7, B7 and ±20 c; prints real freq, detected freq, cents error, clarity; flags octave errors), **timing** (µs per analysis, mean and max of 50), **live mic** (continuous capture, ~10 lines/s with `note, cents, Hz, clarity, rms, analyses/s, loop ms`). All text in English. Choose 16 or 32 kHz from the data (§4.1 criterion) and note it in `TONEMAP_DESIGN.md`. Build both envs, commit, push.
- **Phase 4:** `tonemap_model` (§3.2–3.4, §4.4), pure, `nowMs` as parameter. Bench tests with `PASS`/`FAIL` per series: **note parity** (from `tonemap.live/tests/fixtures/temperament-vectors.json`, rows with `temperament = equal` and `stretch = none`, 264 rows, A4 = 440 and 441; a Python script generates `test/tonemap_vectors.h` with `inputHz, a4, expect.midi, expect.cents, expect.needleEven[0]`; check exact midi, cents ±0.01, needle position ±0.001); **color** (known errors per mode → expected color of §3.3: inside dead zone, at the edge, mid-ramp, saturated, sign); **history** (1 s at +10 c then 2 s at −10 c → expected mean for History 1 s, 3 s, 30 s, unlimited); **fade** (hold 5 s → intensity at 4, 10, 20 s; no fade); **re-basing** (A4 442 → 440 with history at +0 c → mean ≈ +7.85 c); **transposition** (C → Bb moves the cell 2 semitones up).
  **STOP A**: Sergio installs the bench firmware (WebUI, or USB with permission); Claude reads serial. (1) Synthetic and parity results all `PASS`. (2) Live mic: Sergio plays a slow scale, long notes, C3–C7; check no octave errors, stable cents (±2 c on a sustained note), ≥ 25 analyses/s. (3) Calibrate noise gate thresholds (silent room, talking, playing piano/forte) and clarity. (4) Record measured values in `TONEMAP_DESIGN.md`.
- **Phase 5:** `tonemap_view` per §2.2–2.8 (grid, active cell, needle, strip, messages, keys, LED with brightness, pause); integration in `mode_tuner.cpp`, `onReselect` in `ui.h`/`main.cpp`, settings in `settings.*` (§4.7). Env `cardputer-adv-tonemap-debug` (`-DTONEMAP_DEBUG`): an overlaid line with analyses/s, analysis ms, draw ms, rms and clarity, and a serial log twice per second. Build all envs, commit, push.
  **STOP B**: Sergio tests on the device with the list of §6 B; Claude follows the debug env's serial if installed.
- **Phase 6 (new `general-purpose`, no edits):** review a finished feature it did not write. Read this brief, `TONEMAP_DESIGN.md`, `SPEC.md`, `CLAUDE.md` and the diff of `feature/tonemap` vs `main`. Check: (1) every requirement of §2 and §6 implemented; (2) formulas match §3 and the tonemap.live source; (3) CLAUDE.md rules (English UI text, silent UI, no new deps, classic tuner/metronome/game/About unchanged); (4) bugs: integer overflow in ms/chunk math, ring buffer indices, NaN/inf on silence, LED/brightness not restored on every exit path, mic/drone state on view and mode switches. Report findings ranked by severity with file:line and a concrete failure scenario.
- **Phase 7 (orchestrator):** fix phase 6 findings and whatever STOP B revealed; build, commit, push. **STOP C**: Sergio's final test with all of §6; tick the boxes in `docs/SPEC.md`; ask Sergio whether to merge `feature/tonemap` into `main`.

---

## 6. Acceptance criteria

**A. Bench (serial)**
- [ ] Note parity with the ToneMap fixture: 264/264 rows `PASS`.
- [ ] Synthetic: no octave errors from C2 to B7 (sine and sawtooth); error ≤ ±1 c from C2 to C6 and ≤ ±3 c in octave 7 (or documented "best effort").
- [ ] Full analysis in < 20 ms and ≥ 25 analyses/s live with the screen drawing.
- [ ] Color, history, fade, re-basing and transposition tests: `PASS`.

**B. On the device (Sergio)**
- [ ] `T` inside the tuner toggles views; returning from another mode opens the last used (also after reboot).
- [ ] A slow scale paints each note in its cell; a deliberately high note tints orange, low blue, centered green.
- [ ] The active cell's needle follows a vibrato; the strip's cents turn green when centered.
- [ ] `S`, `H`, `C`, `N`, `K`, `F`, `L`, `R` and `Space` do what §2.5 says, show their message and are remembered after reboot.
- [ ] Up/Down change A and it is the same as in the classic view. The map is re-scored when it changes.
- [ ] In silence nothing is painted with the gate at `Low`; talking close, little or nothing with `High`.
- [ ] The map is still there after going to the metronome and back; `R` clears it.
- [ ] Green LED when centered, off otherwise. With `L` off or on exit, brightness returns to the user's.
- [ ] No sounds on key presses. All text in English.
- [ ] Classic view, drone, metronome, game and About work exactly as before.

---

## 7. Known risks

- **Built-in mic bass:** may not pick up well below ~C3. STOP A will tell. If it fails, the C2 row stays empty but the grid is still C2–B7, as Sergio asked.
- **Octave 7 at 16 kHz:** imprecise; hence the 32 kHz measurement.
- **CPU:** full-range detector + drawing + 30 analyses/s may choke the loop. Outs are in §4.2 and §4.5.
- **LED and brightness:** brightness must be restored on every exit path (view change, mode change, `L`).
- **Capture gaps:** if M5Unified's two-recording queue is not kept full, `dt` will be irregular. The bench detects it with analyses/s.

## 8. Decisions from the conversation (2026-10-16)

- v1 scope: core + basic settings (sensitivity, History, Color hold) + noise gate, transposition, sharps/flats and LED.
- Main instrument: strings / high winds.
- Screen: 6 octaves C2–B7 with info strip; labels only the note (octave in the C column).
- Entry: `T` toggles inside the tuner; the last view is remembered.
- One key per setting; `R` clears; Up/Down change A.
- The map lasts while the device is on.
- LED with key `L`, on by default.
- Execution by phases with stops; brief in the repo (branch) and in the claude.ai project.
- Decided by Claude (changeable): `Space` = pause; Left/Right unused; the bar still says `Tuner`; default gate `Low`; transpositions C/Bb/Eb/F with ToneMap's values.

## 9. Changes after device tests (2026-10-22)

Sergio tested the view on the Cardputer ADV and asked for a map more readable at a glance. Deliberate differences from tonemap.live and the sections above (detail and reasons in `TONEMAP_DESIGN.md` §13):

- **Active cell:** the inner white 2 px border covered too much of the 20 × 18 px cell; now (second batch, 2026-10-22) white is exclusive to the active cell: a 2 px white bar on the cell's bottom edge and the label in white (no corners, outline or shadow); other labels black or light gray `(190,190,190)` by luminance. The black 2 px reference line and the needle do not change (the needle is drawn after the bar).
- **Palettes** (§2.3, replaces the fixed palette): `Safe` (default; blue – near white – orange, for color blindness, luminance ordered center > high > low) and `Extreme` (formerly `Classic`; pure primaries in one continuous gradient: blue at the low end, green exactly at the center, red at the high end, no plateau or jumps — linear `t_lin` over the whole range, ±15.5 c in Medium — and no dimming below 75 %, see `TONEMAP_DESIGN.md` §13.7). Key `P`, setting `tmPal`, message `Palette: Safe` / `Palette: Extreme`. Labels of touched cells become black or light gray by the shown color's luminance (white only in the active cell).
- **Narrower ramp** (§3.3): `span` = 2·dz in Relaxed and Medium, 4·dz in Hard (formerly 4, 4 and 8). Full color from ±18.5 c (Relaxed), ±15.5 c (Medium) and ±10.1 c (Hard); the pure-center zone is unchanged (±6.2 / ±5.2 / ±2.0 c).
- **Full intensity at 250 ms** of sound (`FULL_CONF_MS`, formerly 500) and **fade floor 0.45** (`STAIN_MIN_FRACTION`, formerly 0.25) (§3.4).
- The normal firmware (`cardputer-adv`) has no debug line or overlay: that only exists in `cardputer-adv-tonemap-debug`.
- **Options panel** (2026-10-22, second batch): `Alt` or `Enter` toggles a full-screen overlay with the keys and their current values (`SPEC.md`, `TONEMAP_DESIGN.md` §13.5). `Alt:menu` hint in the info strip when it fits.
- **Cents and LED in live color** (2026-10-22, third batch): the strip's cents text and the LED show the color of the sounding note's instantaneous deviation, in the active palette (Safe: with dead zone; Extreme: continuous gradient), at full intensity. Replaces the "green text / green LED inside the dead zone" of §2.4 and §2.8 (`TONEMAP_DESIGN.md` §13.8).
