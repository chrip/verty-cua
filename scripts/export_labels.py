#!/usr/bin/env python3
"""Freeze the hand-labelled ground truth into a prediction-independent file.

    python3 scripts/export_labels.py review/ -o labels.json

Labels live as sheets sorted into review/{expected,unexpected,unknown}/, and the
sheet filename carries the verdict that was PREDICTED when it was rendered. That
is fine for reviewing and useless for iterating: change a rule and every
prediction moves, so every label reads as stale even though the human judgement
about what the action did has not changed at all.

So the truth is exported keyed by the identity of the STEP -- trajectory, index,
action and coordinate -- with the prediction dropped. Any later run can then be
scored against it without re-labelling, which is what makes it possible to
measure a rule change instead of arguing about it.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

VERDICTS = ("expected", "unexpected", "unknown")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("review_dir")
    ap.add_argument("-o", "--out", default="labels.json")
    args = ap.parse_args()

    review = Path(args.review_dir)
    labels: dict[str, str] = {}
    clashes = 0
    for truth in VERDICTS:
        for p in (review / truth).glob("*.png"):
            parts = p.name[:-4].split("__")
            if len(parts) < 3:
                continue
            key = f"{parts[1]}::{parts[2]}"      # trajectory :: step_action_XxY
            if key in labels and labels[key] != truth:
                print(f"  conflicting labels for {key}: {labels[key]} vs {truth}",
                      file=sys.stderr)
                clashes += 1
            labels[key] = truth

    Path(args.out).write_text(json.dumps(
        {"schema": 1,
         "note": "hand-labelled ground truth; key is trajectory::step_action_XxY",
         "labels": labels},
        indent=1, sort_keys=True))
    counts = {v: sum(1 for t in labels.values() if t == v) for v in VERDICTS}
    print(f"{len(labels)} labels -> {args.out}   {counts}")
    if clashes:
        print(f"  {clashes} conflict(s) -- last one wins", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
