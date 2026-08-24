#!/usr/bin/env python3
"""Failure lead time: how many actions before the harness knows, does Verty know?

    python3 scripts/lead_time.py <runs_dir> [--stuck N]

A benchmark harness reports one bit at the end of a trajectory: did the task
succeed. That verdict arrives after the whole action budget is spent, and says
nothing about when the run became unrecoverable. The question here is whether
cheap visual signals know earlier.

For each trajectory this records the first action at which each signal fires,
and compares it against the action at which the run ended:

    lead = actions_total - action_of_first_signal

A signal is only useful if it also stays quiet on runs that succeed, so the
false-alarm rate is reported beside the lead time. A detector that fires on
everything has perfect recall and no value; the two columns have to be read
together.

Failure is taken as: verification did not pass, OR the run exhausted its turn
budget. A run that burns its whole budget has failed whatever the summary says.
"""
from __future__ import annotations

import argparse
import json
import statistics as st
import sys
from pathlib import Path

SIGNALS = ("unexpected", "no_progress", "external", "any")


def analyse(run: Path, stuck_n: int) -> dict | None:
    f = run / "events.jsonl"
    if not f.is_file():
        return None
    ev = [json.loads(l) for l in f.read_text().splitlines() if l.strip()]
    checks = [e for e in ev if e["type"] == "verty_check"]
    if not checks:
        return None

    verified = None
    for e in ev:
        if e["type"] == "verification_completed":
            verified = "passed" if "passed" in (e["message"] or "").lower() else "failed"
    turns = sum(1 for e in ev if e["type"] == "model_turn_started")
    max_turns = 50
    outcome = "passed" if verified == "passed" else "failed"
    if turns >= max_turns:
        outcome = "failed"   # exhausted the budget; the summary is not the truth

    n = len(checks)
    first = {k: None for k in SIGNALS}
    for i, e in enumerate(checks):
        d = e.get("detail") or {}
        if first["unexpected"] is None and d.get("verdict") == "unexpected":
            first["unexpected"] = i
        if first["no_progress"] is None and d.get("no_novel_run", 0) >= stuck_n:
            first["no_progress"] = i
        if first["external"] is None and d.get("external_change"):
            first["external"] = i
    fires = [v for k, v in first.items() if k != "any" and v is not None]
    first["any"] = min(fires) if fires else None
    return {"run": run.name, "outcome": outcome, "actions": n, "first": first}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs_dir")
    ap.add_argument("--stuck", type=int, default=3,
                    help="consecutive no-new-content actions that count as stuck")
    args = ap.parse_args()

    rows = [r for r in (analyse(d, args.stuck) for d in sorted(Path(args.runs_dir).iterdir())
                        if d.is_dir()) if r]
    failed = [r for r in rows if r["outcome"] == "failed"]
    passed = [r for r in rows if r["outcome"] == "passed"]
    if not failed:
        print("no failed trajectories found")
        return 1

    print(f"{len(rows)} trajectories: {len(passed)} passed, {len(failed)} failed"
          f"   (stuck threshold = {args.stuck})\n")
    print(f"{'signal':<14}{'caught':>12}{'false alarms':>15}{'median lead':>14}{'max lead':>10}")
    print("-" * 65)
    for sig in SIGNALS:
        hit = [r for r in failed if r["first"][sig] is not None]
        leads = [r["actions"] - r["first"][sig] for r in hit]
        fa = [r for r in passed if r["first"][sig] is not None]
        name = "ANY" if sig == "any" else sig
        print(f"{name:<14}{f'{len(hit)}/{len(failed)}':>12}"
              f"{f'{len(fa)}/{len(passed)}':>15}"
              f"{(st.median(leads) if leads else 0):>14.0f}"
              f"{(max(leads) if leads else 0):>10}")

    print("\nper failed trajectory (lead in actions before the run ended):")
    print(f"  {'run':<14}{'actions':>8}{'unexpected':>12}{'no_progress':>13}{'external':>10}")
    for r in sorted(failed, key=lambda r: -r["actions"]):
        def lead(s):
            v = r["first"][s]
            return f"{r['actions'] - v}" if v is not None else "-"
        print(f"  {r['run'][:12]:<14}{r['actions']:>8}{lead('unexpected'):>12}"
              f"{lead('no_progress'):>13}{lead('external'):>10}")

    print("\nRead the first two columns together. A signal that fires on most")
    print("successful runs is not detecting failure, it is detecting activity.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
