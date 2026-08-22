#!/usr/bin/env python3
"""Score `external_change` against a lab that logs its own interruptions.

    python3 scripts/score_external.py <run_dir>

`external_change` is the one signal in the system that shipped without ground
truth. It claims to separate "this change is the action's doing" from "something
else changed the screen" -- the case that matters for GUI testing, where a
notification or another editor's keystroke arrives mid-run.

The `interrupted-checklist` lab makes that scoreable. It appends to an activity
feed on a fixed timer, independent of the agent, and records the wall-clock time
of every append. So for each agent step there is a fact of the matter: an
injection either landed inside the step's before/after window or it did not.

Truth here is *an interruption occurred during this step*, not *the detector
should have fired*. Those differ: an injection landing while the screen was
already changing for other reasons may be genuinely unattributable. The
confusion matrix is the interesting output, not a single score.
"""
from __future__ import annotations

import ast
import json
import re
import sys
from datetime import datetime
from pathlib import Path


def parse_ts(s):
    return datetime.fromisoformat(str(s).replace("Z", "+00:00")).timestamp()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__); return 2
    run = Path(sys.argv[1])
    rows = [json.loads(l) for l in (run / "events.jsonl").read_text().splitlines() if l.strip()]

    # Injection timestamps ride in the verification detail.
    # The verification EVENT carries the detail; run.json does not reliably.
    # A `run_cancelled` emitted during process teardown rewrites the summary and
    # takes verification_detail with it, even for runs that passed. The event
    # stream is append-only and keeps it.
    stamps: list[int] = []
    detail = ""
    for e in rows:
        if e["type"] == "verification_completed":
            d = e.get("detail")
            detail = d if isinstance(d, str) else (e.get("message") or "")
    # A run that exhausted its turn budget never verifies, but the runner now
    # captures the lab's state on the failure path. That carries the same log.
    if not stamps:
        for e in rows:
            if e["type"] == "lab_state":
                d = e.get("detail") or {}
                got = [int(x.get("at", 0)) / 1000.0 for x in (d.get("injections") or [])]
                if got:
                    stamps = got
    if not detail and not stamps:
        try:
            summary = json.loads((run / "run.json").read_text()).get("summary") or {}
            detail = summary.get("verification_detail") or ""
        except Exception:
            pass
    m = re.search(r"at=(\[[^\]]*\])", detail)
    if m and not stamps:
        stamps = [s / 1000.0 for s in ast.literal_eval(m.group(1))]
    if not stamps:
        print("no injection log found in the verification detail -- was this the "
              "interrupted-checklist scenario?")
        return 1

    # Each verty_check corresponds to one executed action; its window runs from
    # the previous check (or run start) to itself.
    checks = [e for e in rows if e["type"] == "verty_check"]
    if not checks:
        print("no verty_check events -- was QWEN_CUA_VERTY_URL set?")
        return 1
    start = parse_ts(rows[0]["created_at"])

    print(f"{len(checks)} steps, {len(stamps)} injections\n")
    print(f"{'step':>5}{'verdict':>12}{'ext':>6}{'injected':>10}   window")
    tp = fp = tn = fn = 0
    prev = start
    for i, e in enumerate(checks):
        now = parse_ts(e["created_at"])
        d = e.get("detail") or {}
        ext = bool(d.get("external_change"))
        hit = any(prev <= s <= now for s in stamps)
        if ext and hit: tp += 1
        elif ext and not hit: fp += 1
        elif not ext and hit: fn += 1
        else: tn += 1
        print(f"{i:>5}{d.get('verdict','?'):>12}{('yes' if ext else '-'):>6}"
              f"{('yes' if hit else '-'):>10}   {now-prev:5.1f}s")
        prev = now

    print(f"\n  interruption during step, detector fired      {tp:>3}")
    print(f"  interruption during step, detector silent     {fn:>3}")
    print(f"  no interruption, detector fired               {fp:>3}")
    print(f"  no interruption, detector silent              {tn:>3}")
    if tp + fp:
        print(f"\n  precision {100*tp/(tp+fp):.0f}%  (when it fires, was there an interruption)")
    if tp + fn:
        print(f"  recall    {100*tp/(tp+fn):.0f}%  (of interrupted steps, how many it caught)")
    print("\n  Note: an interruption inside the window does not oblige the detector to")
    print("  fire -- a feed update during a large agent-caused change is genuinely")
    print("  unattributable. Read the matrix, not a single number.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
