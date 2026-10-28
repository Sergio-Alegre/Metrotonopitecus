# SPEC — Metrotonopitecus

## Final goal

Metronome and tuner for Cardputer ADV, installable from M5Launcher.

## Phase 0 — Hello world on the final workflow (done first)

Goal: validate the whole chain before writing the real app.

1. PlatformIO project for Cardputer ADV (board, M5Cardputer/M5Unified libraries with pinned versions).
2. Minimal program: show "Hello Metrotonopitecus", show the last key pressed, play a short beep (440 Hz) on key press. Print the same over serial.
3. `pio run` builds without errors and produces the application `firmware.bin`.
4. Check the size fits a standard Launcher OTA partition.
5. Sergio installs `firmware.bin` via Launcher WebUI and confirms screen, keyboard and sound.

### Phase 0 acceptance criteria

- [ ] `pio run` ends with no errors or relevant warnings
- [ ] Binary fits the Launcher OTA partition
- [ ] Installed via Launcher: boots, shows the text, reacts to the keyboard
- [ ] Can return to Launcher without problems
- [ ] README or `docs/DEPLOY.md` with the deployment procedure that actually worked

### Out of scope in Phase 0

Metronome, tuner, frequency detection, menus, persistent settings.

## Phase 1 — Interface and modes (first skeleton)

Screen 240x135 (rotation 1). All text in English (see CLAUDE.md). Silent interface.

### Structure

- Bottom bar: one text line + 1 px line above: `M:Metro T:Tuner G:Game A:About`, active mode highlighted, volume on the right.
- **Battery icon** at the far right of the bar, next to the volume (13 × 7 px, no percentage; the `Vol` text shifts left by the icon width, and drops its space, `Vol10`, only if it does not fit next to the modes). 1/5 precision: five segments of 20 % (`ceil(level / 20)`, at least 1 while level > 0, 0 at level 0). Color: normal = color of the `Vol` text; **exactly 2 segments = yellow**; **1 or 0 = red** (outline and segments); never green. Reading unavailable (level < 0 or implausible voltage) = gray outline, no segments. If the board reported charging, the outline would be white (ADV gives no such data, see `TONEMAP_DESIGN.md` §14). Read every ~5 s, with median and persistence (see §14); the bar is redrawn only when the segment count changes.
- Global keys: `M` metronome, `T` tuner, `G` game, `A` extra (name TBD; "About" for now).
- Global volume (metronome click and reference note): `-`/`_` down, `=`/`+` up.
- Metronome click sound: all clicks at 1400 Hz; importance is marked by volume (per-click gain over global volume) and a little by duration: first beat of the bar 100 % and 36 ms, other beats 75 % and 30 ms, subdivisions 55 % and 18 ms (constants `kGain*`/`kMs*` in `mode_metronome.cpp`, to be tuned by ear).
- Cursors = keys `;` (up) `,` (left) `.` (down) `/` (right), with or without Fn.
- Persisted in NVS: volume, BPM, subdivision, target note and reference A.

### Metronome (generic, to be shaped)

- Digits: type BPM (Del erases; confirmed with Space, any other key, at the 3rd digit, or after a 2.4 s pause). Up/Down: BPM ±1 (hold = repeat).
- `Enter`: two-press tap tempo, no timers, designed for the right hand (with Space and cursors). 1st press opens a measurement (number in yellow), 2nd closes it and sets BPM from the time between both; 3rd opens a new one, so nothing carries over. Any other key cancels an open measurement. Interval < 200 ms is ignored; > 3 s counts as a new opening. A half-typed number is confirmed first. On-screen hint: `Enter: tap`.
- Left/Right: subdivision (1, 2, 3 = triplet, 4 per beat). Space: start/stop.
- `B`: beats per bar, 1 to 6, rotating (after 6 back to 1). Default 4; saved in settings. One circle per beat, left-justified on the bottom row (6 fit next to the "B: beats" hint on the right). Accent on the first.
- Built-in RGB LED (GPIO 21, max brightness): flash on every beat (not subdivisions), white on the accent and green on the others (color-blind-safe pair, simulated min ΔE 57 vs 28 for red/green; on-screen circles use the same colors; the accent flash lasts 1.5× longer, a cue independent of color). Lit right after the frame with the lit circle finishes sending to the display, so light and circle coincide. Duration = 1/5 of the beat clamped to 30–80 ms (80 ms up to 150 BPM, 40 ms at 300 BPM). On stop, the mean click→LED delay is printed over serial.
  Hardware: the LED is on GPIO21 and driven with `neopixelWrite` (`M5.Led` does nothing with this Arduino core). Its power is GPIO38, the same pin as the screen backlight (PWM), so it only works with the screen at high brightness: measured, works at 192 and fails at 160 (of 255). Below 255 the pin is cut for part of each PWM period and a command sent then is lost (flashes that never end). Metronome mode sets brightness to 255 while active and restores it on exit, and repeats every LED command twice (at 6 and 12 ms). The `cardputer-adv-led-debug` env adds a diagnostic line and L/K keys to test the LED.

### Tuner

- **Automatic nearest-note detection** over the whole range C2..B7 (written note = concert note, no transposition): the tuner hears whatever sounds, finds the nearest note (equal temperament, A = `refA`, the same `tonemap::freqToNote` as the ToneMap view) and the needle shows how many cents above/below **that note** you are.
- Needle over a ~100° arc (analog meter style), center = correct note, green band ±5 cents, scale ±50 cents. With no valid reading the needle returns to rest (left, gray).
- **Detected note**: large at the dial center (name with drawn `#`, and octave, e.g. `A#4`; white, green within ±5 cents; a dim `--` while nothing is heard). Top center: signed cents (`+3c`) and real frequency in Hz.
- Up/Down: reference A in Hz (default 442, range 400–480, persistent, shown on screen).
- Left/Right: **only selects the drone note**, in semitone steps, octaves 3 to 5 (C3–B5); no longer limits detection. `Drone <Hz>` shown under the selector.
- Space or `P`: drone (sine wave) at the chosen note's frequency.
- Engine: the ToneMap view's (continuous capture at 32 kHz in `tonemap_audio`, 2048-sample window, `pitch_mpm` with hybrid K and maxLag 720, **automatic noise gate** (rms ≥ 2.5 × room noise floor, between 20 and 80; see below), clarity > 0.8, 14 < Hz < 8200; shared code in `tonemap_detect.*`). One new reading per window (~31/s); exponential smoothing of ~185 ms while the note does not change (a note change or a jump over 25 cents is taken instantly) and the reading is held 0.7 s without valid windows before the needle returns to rest.
- **Automatic noise gate (classic view)**: no key, no setting. The noise floor is the minimum rms seen (drops instantly to a quieter window, never below 1) and rises slowly (30 s time constant) toward louder windows; gate = `2.5 × floor`, clamped to **20..80**. In a silent room (floor ~6) the gate is 20, so a muted instrument is detected (rms playing p5 ≈ 27); in a noisy room it rises to the old 80 and never more. Updated with **all** windows (also those under the gate) and reset on entering/leaving and when the drone starts/stops, not on every lost reading. The ToneMap view keeps its manual gate (table below). See `TONEMAP_DESIGN.md` §16.
- On the ADV, mic and speaker share the I2S clock: with the drone playing the mic is deaf (needle and dial go gray, center says `Mic off (drone playing)`).
- **LED** (key `L`, same `tmLed` setting as the ToneMap view, default on): the RGB LED (GPIO 21) turns **green `(0,255,0)`** while the needle is inside the green band (|cents| ≤ 5 using the smoothed value the view already uses to color note and cents green) and **off** the rest of the time (no red or other colors): no reading, drone playing (mic off), mic unavailable, LED off, on changing view, mode or screen. Same hardware restriction as the metronome: only works with screen brightness 255, so with `L` on the view saves the user's brightness, raises to 255 and restores it on leaving the view or mode, or when `L` goes off; each command repeated twice. Pressing `L` shows `LED: on` / `LED: off` in the footer for 1.5 s instead of `Space: drone`. Shared code with the ToneMap view in `tuner_led.*` (`TONEMAP_DESIGN.md` §14).

### Tuner — ToneMap view

Second tuner view, ported from [tonemap.live](https://github.com/crossrhythm/tonemap.live) (MIT). Full brief, technical reference and phase plan: [`TONEMAP_BRIEF.md`](TONEMAP_BRIEF.md). Branch `feature/tonemap`. Acceptance criteria (brief §6) are marked at the end of this section.

#### Entering and leaving

- Inside the tuner, **`T` toggles** between the classic view (needle + automatic detected note) and the **ToneMap** view. Today `T` inside the tuner does nothing.
- Arriving at the tuner from another mode opens **the last view used** (persisted in NVS).
- The bottom bar is unchanged: `T:Tuner` stays highlighted. Other global keys unchanged (`M`, `G`, `A`, volume `-`/`=`).
- Entering ToneMap with the drone playing stops the drone and starts the mic (mic and speaker are mutually exclusive on the ADV). **No drone** in ToneMap.

#### Screen layout (content canvas 240 × 125)

```
y=0   ┌─────────────────────────────────────────────┐
      │ A4  +3.2c  442.6 Hz        Bb  A=442  ▮▮▮▯ │  info strip, Font0 (6×8 px per char)
y=15  ├─────────────────────────────────────────────┤  1 px dark gray line
y=17  ├───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┤
      │C7 │C# │D  │D# │E  │F  │F# │G  │G# │A  │A# │B  │  6 rows × 18 px = 108 px (y 17..124)
      │C6 │   │   │   │   │   │   │   │   │   │   │   │  12 columns × 20 px = 240 px
      │C5 │   │   │   │   │   │   │   │   │   │   │   │  top row = highest octave
      │C4 │   │   │   │▒▒ │   │   │   │   │███│   │   │
      │C3 │   │   │   │   │   │   │   │   │   │   │   │
      │C2 │   │   │   │   │   │   │   │   │   │   │   │
y=125 └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
      (mode bar of main.cpp, unchanged)
```

Exact strip pixels are up to the implementer; the grid is fixed: 20 × 18 px per cell, C2 bottom-left, B7 top-right. Fixed range in v1 (written MIDI 36..107).

#### Cells

- Grid lines 1 px `#374151`. Background `#000000`.
- **Label:** note name only (`C#`, or `Db` with flats); the C column also carries the octave (`C4`). Font0. Color `#4b5563` while the cell was never touched; once touched (`visited`), **black or light gray `(190,190,190)` by relative luminance of the color the cell shows** (0.2126 R + 0.7152 G + 0.0722 B over encoded sRGB channels, 0..1; above 0.5 → black, else light gray), so it reads on the near-white center of the Safe palette and on dark cells (faded or little played). **Pure white is used only by the active cell** (its label and bar, see below); it appears nowhere else in the grid.
- **Background color** = blend between black and the trend color by intensity (§3.4). The model gives a signed trend `t` in [−1, +1] (+ = high/sharp, 0 = center) and a **palette** turns it into color (all colors at full intensity; the blend with black by intensity stays):
  - **Safe** (0, default; for red-green and blue-yellow color blindness): low `rgb(0,100,255)`, center `rgb(200,255,200)` (near white with a green touch), high `rgb(255,110,0)`; linear RGB interpolation center→high and center→low. Luminance strictly ordered center > high > low, so it also works in grayscale.
  - **Extreme** (1; formerly "Classic"; ignores color blindness, Safe handles that): pure primaries as far apart as possible in CIELAB (ΔE76 green–blue 259, blue–red 176, green–red 171). **One continuous gradient** over the whole cell range: low end = pure blue `(0,0,255)`, exact center = pure green `(0,255,0)`, high end = pure red `(255,0,0)`, via cyan `(0,255,255)` and yellow `(255,255,0)` halfway on each side (max-luminance midpoints avoid brown). No green plateau or jumps: position is linear, `t_lin = clamp(e' / (dz·(1 + span)), −1, +1)` with `e'` = mean error × `errScale` (`biasTLinear`; Safe keeps the `biasT` dead zone). In cents: full blue/red at ±18.5 (Relaxed), ±15.5 (Medium), ±10.1 (Hard); yellow/cyan at ±9.3 / ±7.8 / ±5.0. Also **no dimming** (a cell with accumulated sound is painted at intensity `0.75 + 0.25·display`, never below 75 %; visited but never stained cells stay black and faded ones stay at `0.75 + 0.25·0.45` = 0.8625). See `TONEMAP_DESIGN.md` §13.7.
  - **Ramp** (narrower than tonemap.live, decided after on-device tests): pure center up to ±6.2 c (Relaxed), ±5.2 c (Medium), ±2.0 c (Hard); full color from ±18.5 c, ±15.5 c and ±10.1 c respectively (`span` = 2·dz, 2·dz and 4·dz). Full intensity at **250 ms** of accumulated sound, and fade goes down to **0.45** of base intensity (ToneMap: 500 ms and 0.25). See `TONEMAP_DESIGN.md` §13.
- **Active cell** (the note sounding now, after the gates of §3.2). White exists only here and in two places: (a) a **white bar** 2 px high across the cell inside the grid lines (19 px: x..x+18) resting on the bottom edge (rows y+15 and y+16 of the 20 × 18 cell; the bottom grid line is row y+17); and (b) the **note name label in white**. No outline or shadow. Then a black 2 px reference line at the cell's vertical center and, last, the **needle**: red bar `#ef4444`, 2 px high across the cell (visible where it crosses the white bar). When no longer active, the label returns to black / light gray. Known risk: over the near-white center of Safe `(200,255,200)` the white bar and label contrast little. Vertical position: up = high. Travel from 10 % to 90 % of height for −50…+50 cents (formula in §3.5).

#### Info strip (top row)

- Left: detected note and written octave (`A4`), signed cents with one decimal (`+3.2c`), real frequency in Hz. Cents are painted in the **live color** of the instantaneous deviation of the sounding note (see "LED" below: the active palette, Safe or Extreme, applied to that frame's error; not the cell average), to see at a glance whether you are low or high and which way to correct. Note name stays white and Hz gray. No valid note: `--`.
- Out of range (written note < C2 or > B7): `Below range` / `Above range` instead of the note.
- Right: transposition if not C (`Bb`, `Eb`, `F`), `A=442` (same A as the classic tuner) and a mic level meter (~30 px) with a mark at the noise gate threshold.
- **Transient messages** when a setting changes: for 1.5 s they take the place of note/cents/Hz (right part stays visible): `Sensitivity: Hard`, `History: 30 s`, `Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`, `LED: off`, `Palette: Safe` / `Palette: Extreme`, `Map cleared`. When paused: fixed `Paused (Space)`.

#### Keys in the ToneMap view

| Key | Action |
|---|---|
| `T` | Back to the classic view |
| Up / Down | Reference A ±1 Hz (same `refA` as classic, 400–480, persistent) |
| `S` | Sensitivity: Relaxed → Medium → Hard → … |
| `H` | History: 1 s → 3 s → 30 s → Unlimited → … |
| `C` | Color hold: 5 s → 15 s → 30 s → 2 min → 5 min → No fade → … |
| `N` | Noise gate: None → Min → Low → Med → High → … (ToneMap view only) |
| `K` | Transposition: C → Bb → Eb → F → … |
| `F` | Sharps ↔ flats in labels and strip |
| `L` | Tuning LED on/off |
| `P` | Color palette: Safe ↔ Extreme (redraw only: does not reset timers or touch data). Not a global key |
| `Alt` or `Enter` | Open / close the **options panel** (below) |
| `R` | Clear the map (all cells) |
| `Space` | Pause / resume listening (map freezes; nothing is painted while paused) |
| Left / Right | **Tuning** (temperament): Equal ↔ Ligeti Hora lunga (see "Temperaments"). Changing it **clears the map** |

**Options panel** (`Alt` or `Enter`, toggles): opaque full-screen overlay (240 × 125, Font0, 13 lines of 9 px) with the title `OPTIONS` and `Alt/Enter: close`, and one line per key with its **current value**: `S Sensitivity`, `H History`, `C Color hold`, `N Noise gate`, `K Transpose`, `F Accidentals`, `P Palette`, `L LED` (same texts as the transient messages) and, without value, `R Clear map`, `Space Pause` / `Space Resume` (by state), `Up/Down  A=442 (reference)` and `T Classic view` (the last shares a line with `R Clear map`), plus `</> Tuning` with the current tuning. While open, the map keys keep working and values update instantly; capture, analysis, LED and model keep running underneath, and on close the whole map is redrawn. Redrawn only on a key press (no per-frame cost). Always closed on entering the view; `T`, `M`, `G`, `A` work as usual and close it on leaving. Only the ToneMap view receives `Alt` (`main.cpp` passes it to no other mode). **Not verified on the device** that the keyboard reports `Alt` alone; `Enter` is the safe fallback. In the info strip, left of `A=…` (and of the transposition), the hint `Alt:menu` appears in dim gray, or `Alt` if the whole does not fit, or nothing if it collides with the note / cents / Hz block or a message.

Global keys (`M`, `G`, `A`, `-`, `=`) are still captured by `main.cpp` before the mode: do not use those letters.

#### Settings, defaults and persistence

All saved in NVS with the existing deferred mechanism (`settings::touch()`).

| Setting | Values | Default | NVS key (≤15 chars) |
|---|---|---|---|
| Tuner view | classic / tonemap | classic | `tunerView` |
| Sensitivity | relaxed / medium / hard | medium | `tmSens` |
| History | 1 s / 3 s / 30 s / unlimited | 3 s | `tmHist` |
| Color hold | 5 s / 15 s / 30 s / 120 s / 300 s / no fade | 30 s | `tmHold` |
| Noise gate (ToneMap only) | none (0) / min (30) / low (80) / med (200) / high (450), rms int16 | low | `tmGate` (NVS key `tmGateB`) |

**Gate migration:** `tmGate` used to store the old 4-level scale (0 none, 1 low, 2 med, 3 high). The current 5-level scale is stored in `tmGateB`. On load, if `tmGateB` does not exist and `tmGate` does, it is converted {0, 1, 2, 3} → {0, 2, 3, 4} (`settings::migrateGate`; out-of-range gives Low); afterwards only `tmGateB` is used. With neither key, default is Low (2). Messages: `Noise gate: None` / `Min` / `Low` / `Med` / `High`. The `Min` level (30) is meant for playing with a mute (see `TONEMAP_DESIGN.md` §16).

| Setting | Values | Default | NVS key |
|---|---|---|---|
| Transposition | C (0) / Bb (−2) / Eb (+3) / F (+5) | C | `tmTransp` |
| Accidentals | sharps / flats | sharps | `tmFlats` |
| Tuning LED | on / off | on | `tmLed` |
| Palette | safe / extreme | safe | `tmPal` (0 = Safe, 1 = Extreme) |
| Tuning (temperament) | equal (0) / Ligeti Hora lunga (1) | equal | `tmTemper` |
| Reference A | 400–480 Hz | 442 (exists) | `refA` (exists) |

#### Temperaments (tuning)

ToneMap view only (the classic tuner is always equal temperament). A temperament is a table of 12 offsets in cents relative to equal temperament, per **concert** pitch class (C = 0, all octaves), applied on top of the reference A (`tonemap::kTemperaments`, `latticeOffsetCents`). Each cell's "center" (black reference line), the needle and the nearest-note search are measured against the already shifted note. Shifted notes carry a **small down arrow** in their label (4, 6 or 8 px by shift: < 20, < 40, ≥ 40 cents). Changing temperament clears the map (stored errors were against other targets).

- **Equal**: all 0.
- **Ligeti Hora lunga**: Sonata for solo viola (1994), 1st movement. Score footnote: three down arrows imitate the harmonic series of an imaginary F (the C string serves as model): **B −49** (harmonic 11), **Eb −31** (harmonic 7) and **A −14** (harmonic 5, the difference between just and tempered major third). Other notes, normal tuning. Not a historical scale with its own name (searched): a scheme of this movement. Values = the score's (theoretical: −48.7 / −31.2 / −13.7).

Pending: more temperaments and a user-defined one (editable 12-offset table); tonal center stays fixed.

Transposition values are ToneMap's (semitones; written note = concert note − value, see §3.2). Adding A (clarinet in A, −3) is trivial if Sergio asks later.

#### The heat map

- Behavior = ToneMap (§3). The map lives in RAM **while the device is on**: survives switching mode or view and coming back; cleared with `R` or on power off. Not saved to flash or SD.
- Changing A **re-scores** the history (colors recomputed against the new reference), as in ToneMap.
- Changing transposition **shifts** the map (the same concert note lands in another written cell), as in ToneMap. The map stores notes by concert MIDI 0..127, so nothing is lost on transposing: only the shown window (C2–B7 written) changes.
- Changing sensitivity or History recomputes the colors of all cells from the stored history.

#### LED

- With `LED` on: the RGB LED (GPIO 21, `neopixelWrite`) continuously shows the **live color** of the active note: the color of its **instantaneous** deviation (the frame's raw `errRatio`, not the cell's weighted average nor Relaxed's smoothed needle) in the **active palette**, at full intensity (no dimming or blend with black). Safe: `biasT` (with its dead zone: inside, center color `(200,255,200)`) → `paletteColor(0, t)`; Extreme: `biasTLinear` (continuous gradient, exact 0 = pure green) → `paletteColor(1, t)`. This rule replaces the old "green only inside the dead zone". Off when there is no note, when paused, with `L` off and on exit.
- To avoid flooding the LED, the new color is sent at note start and afterwards only when some channel changes by 16 levels or more and at most every 50 ms; each send is repeated twice (at 6 and 12 ms) as before.
- Known hardware restriction (see SPEC, metronome): the LED shares the backlight pin and **only works with screen brightness 255**. With LED on, the ToneMap view saves the user's brightness, sets 255 and restores it on leaving the view or mode, or when LED goes off. Repeat each LED command twice (at 6 and 12 ms), as the metronome does.
- The metronome's LED code is untouched. LED and brightness primitives (send with repeats, blocking off, raise/restore brightness) live in `tuner_led.*`, shared with the classic tuner view.

#### Out of scope in v1

More temperaments (Equal and Ligeti Hora lunga exist, see above) and tonal center, Railsback stretch, Note View (Ribbons/Balloons), recorder and WAV, palette editor, AGC, statistics, saving the map to SD, changing the visible range. The architecture must not prevent adding temperaments later (variable-width columns): frequency → note conversion must go through a single "target offset per note" function, as in ToneMap.

#### Acceptance criteria

See brief §6. Status (2026-10-23):

**A. Bench (serial)**
- [x] Note parity with the ToneMap fixture: 264/264 rows `PASS` (plus 192 extra own rows).
- [~] Synthetic: no octave errors from C2 to B7 at 32 kHz. Error ≤ 1 c from C2 to C6 with sine; with sawtooth C6 +20 c gives +1.39 c. Octave 7: sine ≤ 1.65 c; sawtooth up to +8.7 c → **"best effort" documented** (TONEMAP_DESIGN §12).
- [x] Full analysis in < 20 ms (9.25 ms measured). ≥ 25 analyses/s with the screen drawing: **31.2/s mean** (min 29.8) with real analysis and drawing for 75 s playing viola (env `cardputer-adv-tonemap-debug`, noise gate None); 9.6 ms/analysis, 3.3 ms draw, 12.1 ms send, ~15.6 fps, `starved=0`, `torn=0`, min heap 216 KB.
- [x] Color, history, fade, re-base, transposition and live-color tests: `PASS` (14 series on the device).

**B. On the device (Sergio)**: pending STOP C (list in brief §6 B, plus: Alt/Enter menu, `P` palettes, live cents color and LED, automatic classic tuner, white/green metronome with 1400 Hz clicks).

### Game (basic Snake)

- Cursors turn the head; green tube; red dots; dies on wall or own body.
- On death the score blinks; any key restarts. No sound.

## Phase 2 — Tuner, refinement (TBD)

Detection precision, needle stability, microphone sensitivity. Pending device tests.

## Open questions

- Automate deployment through the Launcher WebUI (curl to the upload endpoint) or keep it manual? Investigate the real WebUI endpoint before deciding.
- Which instruments/tunings and BPM range?
