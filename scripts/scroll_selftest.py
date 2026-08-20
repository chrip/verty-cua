#!/usr/bin/env python3
"""Validate the scroll verification path against exact ground truth.

    python3 scripts/scroll_selftest.py

The recorded CUA corpora contain no scrolling, so `verify_scroll` would
otherwise ship entirely untested -- and a sign error in it would make every
scroll read as "scrolled against the requested direction", which looks exactly
like a real finding.

This builds a corpus where the answer is known by construction: take a tall
stitched page produced by the offline pipeline, crop viewport-sized windows at
chosen offsets, and the scroll between consecutive crops IS the difference of
those offsets. No labelling, no judgement.

It checks two things the rest of the suite cannot:
  * the ScrollDetector recovers dy exactly, including large jumps and upward
    scrolls;
  * `dy > 0` means the page scrolled DOWN, which is what the demo adapter's
    sign flip depends on.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2

ROOT = Path(__file__).resolve().parent.parent
VIEWPORT_H = 800
# Down, small down, big jump, up, and a degenerate no-op.
STEPS = [300, 120, 700, -450, 0]


def main() -> int:
    verty = ROOT / "build" / "verty"
    replay = ROOT / "build" / "verty-replay"
    session = ROOT / "test_data" / "session_jumps" / "session.mp4"
    for p in (verty, replay):
        if not p.is_file():
            print(f"missing {p} -- build first", file=sys.stderr)
            return 2
    if not session.is_file():
        print(f"missing corpus {session}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        page = tmp / "page.png"
        subprocess.run([str(verty), str(session), "-o", str(page)],
                       check=True, capture_output=True)
        src = cv2.imread(str(page))
        if src is None or src.shape[0] < VIEWPORT_H + sum(abs(s) for s in STEPS):
            print("stitched page too short for the test offsets", file=sys.stderr)
            return 2

        offsets, cur = [0], 0
        for d in STEPS:
            cur += d
            offsets.append(cur)

        frames = tmp / "frames"
        frames.mkdir()
        paths = []
        for i, off in enumerate(offsets):
            p = frames / f"{i:03d}.png"
            cv2.imwrite(str(p), src[off:off + VIEWPORT_H, :])
            paths.append(p)

        manifest = tmp / "manifest.tsv"
        with manifest.open("w") as fh:
            fh.write("# source=synthetic_scroll\n")
            fh.write("index\taction\tx\ty\tdy\ttext\tbefore\tafter\n")
            for i, d in enumerate(STEPS):
                fh.write(f"{i}\tscroll\t\t\t{d}\t\t{paths[i]}\t{paths[i+1]}\n")

        out = tmp / "scroll.jsonl"
        subprocess.run([str(replay), str(manifest), "-o", str(out), "--verify"],
                       check=True, capture_output=True)
        rows = [json.loads(l) for l in out.read_text().splitlines() if l.strip()]

    print(f"{'requested':>10} {'measured':>9} {'transition':>12} {'verdict':>10}")
    failures = []
    for r, want in zip(rows, STEPS):
        got = r["dy"]
        print(f"{want:>10} {got:>9} {r['transition']:>12} {r['verdict']:>10}")
        if want != 0 and got != want:
            failures.append(f"step dy: wanted {want}, measured {got}")
        if r["verdict"] != "expected":
            failures.append(f"dy={want}: verdict was {r['verdict']} ({r['reason']})")

    print()
    if failures:
        for f in failures:
            print(f"FAIL  {f}")
        return 1
    print(f"PASS  {len(rows)} scrolls recovered exactly; "
          f"dy>0 == page scrolled down confirmed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
