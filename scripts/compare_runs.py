#!/usr/bin/env python3
"""Build the M4 comparison table from Qwen-CUA demo runs.

    python3 scripts/compare_runs.py /path/to/demo/data/runs
    python3 scripts/compare_runs.py .../runs --label baseline=RUNID --label guard=RUNID

The plan's question is whether a cheap visual layer reduces the cost of a CUA
agent without costing task success. That is not one number: a config that halves
model turns while failing every task is worse, and one that succeeds slightly
more often while doubling tokens is not obviously better either. So all of it is
reported side by side, and nothing is collapsed into a score.

Read from each run's events.jsonl:

  outcome / verification   deterministic, from the lab's own state check
  model turns              every VLM call
  actions                  what the agent actually did
  screenshots              images captured (and so eventually sent)
  tokens                   prompt / completion, per model turn
  wall clock               first to last event
  guard stops              chains Verty aborted (guard configs only)

`verification` is the honest success column. `outcome` is what the model claimed,
and a model claiming success is not evidence that anything succeeded.
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path


def parse_ts(s):
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except Exception:
        return None


def summarize(run_dir: Path) -> dict | None:
    ev = run_dir / "events.jsonl"
    if not ev.is_file():
        return None
    rows = []
    for line in ev.read_text().splitlines():
        if line.strip():
            try:
                rows.append(json.loads(line))
            except Exception:
                pass
    if not rows:
        return None

    run = {}
    rj = run_dir / "run.json"
    if rj.is_file():
        try:
            run = json.loads(rj.read_text())
        except Exception:
            pass

    def count(t):
        return sum(1 for e in rows if e.get("type") == t)

    prompt = sum((e.get("detail") or {}).get("prompt_tokens", 0)
                 for e in rows if e.get("type") == "model_usage")
    completion = sum((e.get("detail") or {}).get("completion_tokens", 0)
                     for e in rows if e.get("type") == "model_usage")

    ts = [parse_ts(e.get("created_at")) for e in rows]
    ts = [t for t in ts if t]
    wall = (max(ts) - min(ts)).total_seconds() if len(ts) > 1 else 0.0

    # The run summary is written at the end and a late cancel event can overwrite
    # it; the event stream is the truth. Prefer verification_completed.
    verified = None
    for e in rows:
        if e.get("type") == "verification_completed":
            verified = "passed" if "passed" in (e.get("message") or "").lower() else "failed"
    completed = any(e.get("type") == "run_completed" for e in rows)

    return {
        "id": run_dir.name,
        "scenario": run.get("scenario_id") or "-",
        "verification": verified or "not_run",
        "completed": completed,
        "model_turns": count("model_turn_started"),
        "actions": count("action_completed"),
        "screenshots": count("screenshot_captured"),
        "prompt_tokens": prompt,
        "completion_tokens": completion,
        "guard_checks": count("verty_check"),
        "guard_stops": count("verty_guard_stop"),
        "wall_s": wall,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs_dir")
    ap.add_argument("--label", action="append", default=[],
                    help="NAME=RUNID, repeatable; unlabelled runs are listed as-is")
    args = ap.parse_args()

    root = Path(args.runs_dir).expanduser().resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1

    names = {}
    for spec in args.label:
        if "=" in spec:
            n, rid = spec.split("=", 1)
            names[rid.strip()] = n.strip()

    rows = []
    for d in sorted(root.iterdir(), key=lambda p: p.stat().st_mtime):
        if d.is_dir():
            s = summarize(d)
            if s:
                s["label"] = names.get(d.name, "")
                rows.append(s)
    if not rows:
        print("no runs found", file=sys.stderr)
        return 1

    hdr = (f"{'label':<12}{'scenario':<24}{'verified':<10}{'turns':>6}{'actions':>8}"
           f"{'shots':>7}{'prompt tok':>12}{'compl tok':>11}{'stops':>7}{'wall s':>9}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        label = r["label"] or r["id"][:10]
        print(f"{label:<12}{r['scenario']:<24}{r['verification']:<10}"
              f"{r['model_turns']:>6}{r['actions']:>8}{r['screenshots']:>7}"
              f"{r['prompt_tokens']:>12}{r['completion_tokens']:>11}"
              f"{r['guard_stops']:>7}{r['wall_s']:>9.0f}")

    labelled = [r for r in rows if r["label"]]
    if len(labelled) >= 2:
        base = labelled[0]
        print(f"\nrelative to `{base['label']}`:")
        for r in labelled[1:]:
            def pct(a, b):
                return f"{100.0 * (a - b) / b:+.0f}%" if b else "  n/a"
            print(f"  {r['label']:<12} turns {pct(r['model_turns'], base['model_turns'])}"
                  f"   actions {pct(r['actions'], base['actions'])}"
                  f"   prompt tok {pct(r['prompt_tokens'], base['prompt_tokens'])}"
                  f"   wall {pct(r['wall_s'], base['wall_s'])}")
        print("\n  Token counts are 0 for runs recorded before usage accounting was added.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
