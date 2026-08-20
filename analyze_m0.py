#!/usr/bin/env python3
"""Summarize a verty-replay run: what do the pixels say CUA actions did?

    python3 analyze_m0.py m0_bridge.jsonl

M0 is a measurement milestone, so this prints distributions rather than
verdicts. The verdict layer is M1 and lives elsewhere on purpose -- the numbers
here are what it has to justify itself against.
"""
from __future__ import annotations

import argparse
import json
import statistics as st
from collections import Counter
from pathlib import Path

TRANSITIONS = ("identical", "scroll", "page_change", "unstable")


def pct(n, d):
    return f"{100.0 * n / d:5.1f}%" if d else "    -"


def bar(frac, width=28):
    return "#" * round(frac * width)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("--change-floor", type=float, default=1e-6,
                    help="changed_frac at or below this counts as no change at all")
    args = ap.parse_args()

    rows = [json.loads(l) for l in Path(args.jsonl).read_text().splitlines() if l.strip()]
    if not rows:
        print("no rows")
        return
    n = len(rows)

    print(f"=== {args.jsonl} -- {n} action steps "
          f"({len({r['trajectory'] for r in rows})} trajectories) ===\n")

    # ── What the inherited classifier says ──────────────────────────────────
    print("Verty transition classes")
    counts = Counter(r["transition"] for r in rows)
    for t in TRANSITIONS:
        c = counts.get(t, 0)
        print(f"  {t:<12} {c:>4}  {pct(c, n)}  {bar(c / n)}")

    # ── What actually changed on screen ─────────────────────────────────────
    # The classifier's Identical test is a FRAME-WIDE mean absolute difference
    # (IDENTICAL_FRAME_MAD = 2.0). A change confined to one dialog cannot move
    # that mean, so `identical` is not the same statement as `nothing happened`.
    # These buckets separate the two.
    print("\nActual change magnitude (share of pixels past SAD_THRESHOLD)")
    buckets = [
        (0.0, args.change_floor, "nothing at all"),
        (args.change_floor, 1e-3, "trace  <0.1%"),
        (1e-3, 1e-2, "local  0.1-1%"),
        (1e-2, 5e-2, "large  1-5%"),
        (5e-2, 1.01, "global >5%"),
    ]
    for lo, hi, name in buckets:
        sel = [r for r in rows if lo <= r["changed_frac"] < hi]
        print(f"  {name:<16} {len(sel):>4}  {pct(len(sel), n)}  {bar(len(sel) / n)}")

    # ── Where the two disagree ──────────────────────────────────────────────
    print("\nCross-tab: transition class vs actual change")
    hdr = f"  {'':<12}" + "".join(f"{b[2].split()[0]:>10}" for b in buckets)
    print(hdr)
    for t in TRANSITIONS:
        sub = [r for r in rows if r["transition"] == t]
        if not sub:
            continue
        line = f"  {t:<12}"
        for lo, hi, _ in buckets:
            line += f"{sum(1 for r in sub if lo <= r['changed_frac'] < hi):>10}"
        print(line)

    # ── Separation: is there a usable gap between local and global? ─────────
    print("\nchanged_frac range per class")
    for t in TRANSITIONS:
        f = sorted(r["changed_frac"] for r in rows if r["transition"] == t)
        if not f:
            continue
        print(f"  {t:<12} n={len(f):>3}  min={f[0]:.5f}  median={st.median(f):.5f}  max={f[-1]:.5f}")

    locals_ = sorted(r["changed_frac"] for r in rows if r["changed_frac"] < 0.05)
    globals_ = sorted(r["changed_frac"] for r in rows if r["changed_frac"] >= 0.05)
    if locals_ and globals_:
        print(f"\n  largest local change  {locals_[-1]:.5f}")
        print(f"  smallest global change {globals_[0]:.5f}")
        print(f"  separation factor      {globals_[0] / max(locals_[-1], 1e-9):.1f}x")

    # ── Click locality: the signal M1's click rule will rest on ─────────────
    # Every click that carried a coordinate -- including the ones that changed
    # nothing, which have no locality fields precisely because there was no
    # change to be near. Selecting on "frac_within_r" in r would drop exactly
    # the steps this milestone most wants to count.
    clicks = [r for r in rows if r["action"] in ("click", "double_click", "right_click")
              and "x" in r]
    if clicks:
        radius = next((r["locality_radius"] for r in clicks if "locality_radius" in r), None)
        print(f"\nClick locality (radius {radius} px, {len(clicks)} clicks with coordinates)")
        dead = [r for r in clicks if r["changed_frac"] <= args.change_floor]
        live = [r for r in clicks if r["changed_frac"] > args.change_floor]
        print(f"  changed nothing at all   {len(dead):>4}  {pct(len(dead), len(clicks))}")
        if live:
            near = [r for r in live if r.get("frac_within_r", 0.0) >= 0.5]
            hit = [r for r in live if r.get("dist_to_change", 1e9) <= 0.0]
            print(f"  >=50% of change near it  {len(near):>4}  {pct(len(near), len(live))}")
            print(f"  click on a changed pixel {len(hit):>4}  {pct(len(hit), len(live))}")
            d = sorted(r["dist_to_change"] for r in live if "dist_to_change" in r)
            print(f"  distance to nearest change  median={st.median(d):.0f}px  max={d[-1]:.0f}px")

    # ── Action vocabulary ───────────────────────────────────────────────────
    print("\nActions")
    for a, c in Counter(r["action"] for r in rows).most_common():
        dead = sum(1 for r in rows if r["action"] == a and r["changed_frac"] <= args.change_floor)
        print(f"  {a:<14} {c:>4}   of which changed nothing: {dead}")

    # ── Cost ────────────────────────────────────────────────────────────────
    ms = sorted(r["ms"] for r in rows)
    print(f"\nPer-step cost   median={st.median(ms):.1f}ms  "
          f"p95={ms[int(0.95 * len(ms))]:.1f}ms  max={ms[-1]:.1f}ms")

    # ── Novel vs revisit, from the per-block history ────────────────────────
    if any("blocks_changed" in r for r in rows):
        b = [r for r in rows if r.get("blocks_changed", 0) > 0]
        if b:
            tn = sum(r["blocks_novel"] for r in b)
            tr = sum(r["blocks_revisit"] for r in b)
            print(f"\nChange by history ({len(b)} steps with changed blocks)")
            print(f"  novel blocks    {tn:>7}  {pct(tn, tn + tr)}   (content never seen before)")
            print(f"  revisit blocks  {tr:>7}  {pct(tr, tn + tr)}   (an earlier appearance returned)")

            pure = [r for r in b if r["blocks_novel"] == 0]
            print(f"\n  steps introducing NO new content at all: {len(pure)} of {len(b)}")

            # A run of steps that introduces nothing new is an agent making no
            # progress -- a cheap, model-free stuck detector.
            runs, cur = [], 0
            for r in rows:
                if r.get("blocks_changed", 0) > 0 and r["blocks_novel"] == 0:
                    cur += 1
                else:
                    if cur >= 2:
                        runs.append(cur)
                    cur = 0
            if cur >= 2:
                runs.append(cur)
            if runs:
                print(f"  consecutive no-new-content runs (>=2): {runs}  "
                      f"longest={max(runs)}")

    # ── M1 verdicts, when the run was made with --verify ────────────────────
    if any("verdict" in r for r in rows):
        v = [r for r in rows if "verdict" in r]
        print(f"\nM1 verdicts ({len(v)} steps)")
        for name in ("expected", "unexpected", "unknown"):
            sel = [r for r in v if r["verdict"] == name]
            print(f"  {name:<12} {len(sel):>4}  {pct(len(sel), len(v))}  {bar(len(sel) / len(v))}")

        print("\n  by action")
        for a in sorted({r["action"] for r in v}):
            sub = [r for r in v if r["action"] == a]
            e = sum(1 for r in sub if r["verdict"] == "expected")
            u = sum(1 for r in sub if r["verdict"] == "unexpected")
            k = sum(1 for r in sub if r["verdict"] == "unknown")
            print(f"    {a:<12} n={len(sub):>3}   expected={e:>3}  unexpected={u:>3}  unknown={k:>3}")

        print("\n  reasons given")
        for reason, c in Counter(r.get("reason", "") for r in v).most_common():
            print(f"    {c:>3}  {reason[:76]}")

        skipped = sum(1 for r in v if r["verdict"] == "expected")
        print(f"\n  VLM turns skipped by this rule set: {skipped}/{len(v)}  "
              f"({pct(skipped, len(v)).strip()})")
        print("  NOTE: these thresholds were fitted on this same corpus, so this")
        print("        figure is descriptive, not a validated precision estimate.")

    # ── The headline the plan asked for ─────────────────────────────────────
    cheap = sum(1 for r in rows if r["changed_frac"] <= args.change_floor)
    localish = sum(1 for r in rows if args.change_floor < r["changed_frac"] < 1e-2)
    print(f"\nVLM turns a cheap check could plausibly have answered:")
    print(f"  nothing changed          {cheap:>4}  {pct(cheap, n)}")
    print(f"  small local change       {localish:>4}  {pct(localish, n)}")
    print(f"  --------------------------------")
    print(f"  combined                 {cheap + localish:>4}  {pct(cheap + localish, n)}")


if __name__ == "__main__":
    main()
