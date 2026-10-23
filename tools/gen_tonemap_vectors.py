#!/usr/bin/env python3
"""Generate test/tonemap_vectors.h for the ToneMap bench (docs/TONEMAP_DESIGN.md section 9).

Python standard library only. Inputs:
  * tonemap.live tests/fixtures/temperament-vectors.json (outside the repo; default
    ../refs/tonemap.live/..., override with --fixture or $TONEMAP_FIXTURE)

Outputs (deterministic, no timestamps):
  * kParityFixture[264]: every temperament=equal / stretch=none row of the fixture
    (inputHz, a4, expect.midi, expect.cents, expect.needleEven[0]).
  * kParityExtra[n]: rows computed in double precision with ToneMap's own formulas
    (freqToNoteData with the +-2 candidate search, getTargetOffsetCents, getNeedleCellPosition
    for equal temperament): negative cents, +-49.9, +-50.1, +-60 at the ends of the MIDI range,
    transpositions 0 / -2 / 3 / 5, extreme MIDI notes 0 / 12 / 107 / 119 / 127, A4 in
    {415, 440, 442, 466}.
  * kNeedleRows[n]: cents -> needle position, including the clamp at +-50 / +-60.

Guard: before the extras are emitted, the script's formulas must reproduce the 264 fixture rows.
The fixture stores inputHz rounded to 6 decimals, so the expected cents of a row (an exact 0 or 30)
can only be reproduced to the effect of that rounding: |dcents| <= 1200/ln2 * 5e-7 / inputHz
(about 1.6e-5 c at 55 Hz, 1e-6 c at 1 kHz). The midi must match exactly.
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_FIXTURE = os.path.normpath(
    os.path.join(ROOT, "..", "refs", "tonemap.live", "tests", "fixtures", "temperament-vectors.json"))
DEFAULT_OUT = os.path.join(ROOT, "test", "tonemap_vectors.h")

A4_REF = 440.0


def f32(x):
    """Round a double to the nearest float32 (what the firmware sees)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def js_round(x):
    """Math.round: ties go up."""
    return math.floor(x + 0.5)


def lattice_offset(midi):
    """getLatticeOffsetCents for equal temperament without stretch."""
    return 0.0


def target_offset(midi, a4):
    """getTargetOffsetCents = lattice + 1200*log2(a4/440)."""
    return lattice_offset(midi) + 1200.0 * math.log2(a4 / A4_REF)


def freq_to_note(freq, a4):
    """freqToNoteData: returns (midi, cents, errRatio, targetFreq)."""
    semis = 12.0 * (math.log(freq / a4) / math.log(2.0))
    pos = semis + 69.0
    midi = js_round(semis) + 69
    best_dev = math.inf
    for cand in range(midi - 2, midi + 3):
        if cand < 0 or cand > 127:
            continue
        dev = abs((pos - cand) * 100.0 - lattice_offset(cand))
        if dev < best_dev:  # strict: ties stay with the lower note
            best_dev = dev
            midi = cand
    equal_temp = A4_REF * 2.0 ** ((midi - 69) / 12.0)
    off = target_offset(midi, a4)
    target = equal_temp * 2.0 ** (off / 1200.0)
    cents = 1200.0 * (math.log(freq / target) / math.log(2.0))
    err = (freq - target) / target
    return midi, cents, err, target


def needle_pct(cents):
    """getNeedleCellPosition for equal temperament (halfBelow = halfAbove = 50): pct."""
    return (max(-50.0, min(50.0, cents)) + 50.0) / 100.0


def fmt_f(x):
    s = "%.9g" % x
    if "e" not in s and "." not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    return s + "f"


def load_fixture(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    data = json.loads(raw.decode("utf-8"))
    rows = data["rows"] if isinstance(data, dict) else data
    return rows, hashlib.sha256(raw).hexdigest()


def select_rows(rows):
    return [r for r in rows if r.get("temperament") == "equal" and r.get("stretch") == "none"]


def guard(rows):
    """The script's formulas must reproduce every fixture row (see module docstring)."""
    worst = 0.0
    for i, r in enumerate(rows):
        hz, a4 = float(r["inputHz"]), float(r["a4"])
        if r.get("transposition", 0) != 0:
            raise SystemExit("guard: unexpected transposition in row %d" % i)
        midi, cents, _err, target = freq_to_note(hz, a4)
        exp = r["expect"]
        if midi != exp["midi"]:
            raise SystemExit("guard: row %d midi %d != fixture %d" % (i, midi, exp["midi"]))
        bound = 1200.0 / math.log(2.0) * 5e-7 / hz * 1.001 + 1e-9
        d = abs(cents - exp["cents"])
        worst = max(worst, d / bound)
        if d > bound:
            raise SystemExit("guard: row %d cents %.9f vs fixture %.9f (bound %.3g)" % (i, cents, exp["cents"], bound))
        if abs(needle_pct(cents) - exp["needleEven"][0]) > 1e-6:
            raise SystemExit("guard: row %d needle %.9f vs fixture %.9f" % (i, needle_pct(cents), exp["needleEven"][0]))
        if abs(target - exp["targetHz"]) > 5e-7 * 1.001:
            raise SystemExit("guard: row %d targetHz %.9f vs fixture %.9f" % (i, target, exp["targetHz"]))
    return worst


def make_extra_rows():
    """Returns [(group, hz_f32, a4, midi_written, cents, needle, transp)]."""
    out = []

    def add(group, concert_midi, offs_cents, a4, transp):
        target = a4 * 2.0 ** ((concert_midi - 69) / 12.0)       # concert target of that note
        hz = f32(target * 2.0 ** (offs_cents / 1200.0))           # what the mic delivers (float32)
        adjusted = hz * 2.0 ** (-transp / 12.0)                   # ToneMap: pitch * 2^(-t/12)
        midi, cents, _e, _t = freq_to_note(adjusted, a4)
        out.append((group, hz, a4, midi, cents, needle_pct(cents), transp))

    # negative cents and +-49.9 (still the same note)
    for m in (45, 60, 69, 81, 96):
        for c in (-1.0, -10.0, -25.0, -49.9):
            add("negative cents", m, c, 440.0, 0)
    for m in (33, 57, 72, 93, 105):
        for c in (49.9, -49.9, 49.0, -33.3):
            add("+-49.9", m, c, 442.0, 0)
    # +-50.1 belongs to the neighbour: midi+1 at -49.9, midi-1 at +49.9
    for m in (48, 60, 69, 84, 100):
        add("+50.1 -> next note", m, 50.1, 440.0, 0)
        add("-50.1 -> previous note", m, -50.1, 440.0, 0)
        add("+50.1 -> next note", m, 50.1, 442.0, 0)
        add("-50.1 -> previous note", m, -50.1, 442.0, 0)
    # extreme MIDI notes (candidates beyond 0..127 are skipped, so +-60 c stays on the end note)
    for m in (0, 12, 107, 119, 127):
        for c in (-40.0, 0.0, 30.0, 49.9):
            add("extreme midi", m, c, 440.0, 0)
    add("extreme midi, clamp", 0, -60.0, 440.0, 0)
    add("extreme midi, clamp", 127, 60.0, 440.0, 0)
    add("extreme midi, clamp", 0, -49.9, 442.0, 0)
    add("extreme midi, clamp", 127, 49.9, 442.0, 0)
    # transpositions 0 / -2 / 3 / 5 (written midi = concert - t)
    for t in (0, -2, 3, 5):
        for m in (45, 57, 69, 81, 100):
            for c in (-20.0, 13.7, 49.0):
                add("transposition %d" % t, m, c, 442.0, t)
    # reference pitches
    for a4 in (415.0, 440.0, 442.0, 466.0):
        for m in (60, 69, 72, 93):
            for c in (-12.3, 7.7, 0.0):
                add("A4 = %g" % a4, m, c, a4, 0)
    return out


def make_needle_rows():
    return [(c, needle_pct(c)) for c in (-60.0, -50.0001, -50.0, -49.9, -25.0, -10.0, 0.0, 5.0, 25.0, 49.9, 50.0, 50.0001, 60.0, 1000.0, -1000.0)]


def render(fixture_rows, extra, needle, digest):
    L = []
    L.append("// GENERATED by tools/gen_tonemap_vectors.py - do not edit by hand.")
    L.append("// Source fixture: tonemap.live tests/fixtures/temperament-vectors.json (sha256 %s)." % digest)
    L.append("// Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)")
    L.append("//")
    L.append("// kParityFixture: the %d temperament=equal / stretch=none rows of the fixture (nominal cents, as in the file)." % len(fixture_rows))
    L.append("// kParityExtra:   double-precision rows computed with ToneMap's formulas (the guard in the generator")
    L.append("//                 first checks that those formulas reproduce the fixture rows).")
    L.append("// transp = ToneMap transposition value (written midi = concert midi - transp); midi is the WRITTEN midi.")
    L.append("#pragma once")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("namespace tmvec {")
    L.append("")
    L.append("struct ParityRow {")
    L.append("    float hz, a4;")
    L.append("    int16_t midi;")
    L.append("    float cents, needle;")
    L.append("    int8_t transp;")
    L.append("};")
    L.append("struct NeedleRow {")
    L.append("    float cents, pct;")
    L.append("};")
    L.append("")
    L.append("constexpr int kParityFixtureCount = %d;" % len(fixture_rows))
    L.append("constexpr int kParityExtraCount = %d;" % len(extra))
    L.append("constexpr int kNeedleRowCount = %d;" % len(needle))
    L.append("")
    L.append("// clang-format off")
    L.append("static const ParityRow kParityFixture[kParityFixtureCount] = {")
    for r in fixture_rows:
        e = r["expect"]
        L.append("    {%s, %s, %d, %s, %s, 0}," % (fmt_f(f32(float(r["inputHz"]))), fmt_f(float(r["a4"])), e["midi"],
                                                  fmt_f(float(e["cents"])), fmt_f(float(e["needleEven"][0]))))
    L.append("};")
    L.append("")
    L.append("static const ParityRow kParityExtra[kParityExtraCount] = {")
    group = None
    for g, hz, a4, midi, cents, needle_v, t in extra:
        if g != group:
            L.append("    // %s" % g)
            group = g
        L.append("    {%s, %s, %d, %s, %s, %d}," % (fmt_f(hz), fmt_f(a4), midi, fmt_f(cents), fmt_f(needle_v), t))
    L.append("};")
    L.append("")
    L.append("static const NeedleRow kNeedleRows[kNeedleRowCount] = {")
    for c, p in needle:
        L.append("    {%s, %s}," % (fmt_f(c), fmt_f(p)))
    L.append("};")
    L.append("// clang-format on")
    L.append("")
    L.append("}  // namespace tmvec")
    L.append("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture", default=os.environ.get("TONEMAP_FIXTURE", DEFAULT_FIXTURE))
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    rows, digest = load_fixture(args.fixture)
    eq = select_rows(rows)
    if len(eq) != 264:
        raise SystemExit("expected 264 equal/none rows, found %d" % len(eq))
    worst = guard(eq)
    extra = make_extra_rows()
    needle = make_needle_rows()
    text = render(eq, extra, needle, digest)
    with open(args.out, "w", newline="\n") as fh:
        fh.write(text)
    print("fixture rows: %d (guard ok, worst cents deviation = %.3f of the fixture-rounding bound)" % (len(eq), worst))
    print("extra rows:   %d" % len(extra))
    print("needle rows:  %d" % len(needle))
    print("wrote %s (%d bytes)" % (args.out, len(text)))


if __name__ == "__main__":
    sys.exit(main())
