#!/usr/bin/env python3
"""Score a verty-replay run against frozen ground truth.

    python3 scripts/score.py m0_all.jsonl labels.json
    python3 scripts/score.py new.jsonl labels.json --diff baseline.jsonl

Prints the confusion matrix and the two numbers that matter:

    precision on `expected`   safety   -- a false Expected is a silent failure
    recall on `expected`      headroom -- savings the rules leave behind

`--diff` shows exactly which steps a rule change moved and in which direction.
A change that gains recall while costing even one point of precision is not an
improvement.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

VERDICTS = ("expected", "unexpected", "unknown")


def key_of(r: dict) -> str:
    loc = f"_{r['x']}x{r['y']}" if "x" in r else ""
    return f"{r['trajectory']}::{r['step']:03d}_{r['action']}{loc}"


def load_run(path: str) -> dict[str, dict]:
    out = {}
    for line in Path(path).read_text().splitlines():
        if line.strip():
            r = json.loads(line)
            if "verdict" in r:
                out[key_of(r)] = r
    return out


def matrix(run, labels):
    m = {p: {t: 0 for t in VERDICTS} for p in VERDICTS}
    n = unlabelled = 0
    for k, r in run.items():
        t = labels.get(k)
        if t is None:
            unlabelled += 1
            continue
        m[r["verdict"]][t] += 1
        n += 1
    return m, n, unlabelled


def report(m, n, unlabelled, title):
    print(f"{title}   {n} scored" + (f", {unlabelled} unlabelled" if unlabelled else ""))
    print(f"{'':>14}{'TRUTH':^36}")
    print(f"{'predicted':>14}" + "".join(f"{t:>12}" for t in VERDICTS))
    for p in VERDICTS:
        print(f"{p:>14}" + "".join(f"{m[p][t]:>12}" for t in VERDICTS))
    tp = m["expected"]["expected"]
    pn = sum(m["expected"].values())
    rn = sum(m[p]["expected"] for p in VERDICTS)
    prec = 100.0 * tp / pn if pn else 0.0
    rec = 100.0 * tp / rn if rn else 0.0
    print(f"\n  precision on `expected`  {tp}/{pn} = {prec:.1f}%")
    print(f"  recall on `expected`     {tp}/{rn} = {rec:.1f}%")
    bad = m["expected"]["unexpected"]
    if bad:
        print(f"  !! {bad} FALSE EXPECTED -- silent task failures")
    return prec, rec


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("labels")
    ap.add_argument("--diff", help="an earlier run to compare against")
    args = ap.parse_args()

    labels = json.loads(Path(args.labels).read_text())["labels"]
    run = load_run(args.jsonl)
    m, n, u = matrix(run, labels)
    prec, rec = report(m, n, u, Path(args.jsonl).name)

    if args.diff:
        prev = load_run(args.diff)
        pm, pn_, pu = matrix(prev, labels)
        print()
        pprec, prec_ = report(pm, pn_, pu, Path(args.diff).name + "  (baseline)")
        print(f"\n  delta: precision {prec - pprec:+.1f} pts   recall {rec - prec_:+.1f} pts")
        moved = [(k, prev[k]["verdict"], run[k]["verdict"], labels.get(k))
                 for k in run if k in prev and prev[k]["verdict"] != run[k]["verdict"]]
        if moved:
            print(f"\n  {len(moved)} step(s) moved:")
            for k, a, b, t in moved[:25]:
                mark = "  GOOD" if b == t else ("  BAD" if a == t else "")
                print(f"    {a:>10} -> {b:<10} truth={t or '?':<10} {k.split('::')[1]}{mark}")
            if len(moved) > 25:
                print(f"    ... and {len(moved) - 25} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
