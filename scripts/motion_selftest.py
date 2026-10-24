#!/usr/bin/env python3
"""Validate block motion and drag verification against exact ground truth.

    python3 scripts/motion_selftest.py

The recorded corpora hold a handful of drags, all on one Kanban board, which is
enough to see the rule work and not enough to trust it. This builds drags whose
outcome is known by construction: cut a textured rectangle out of a real page,
paste it somewhere else, and the motion vector IS the difference of the two
positions. No labelling, no judgement.

Cases, each a short trajectory:
  * a drag that carried the object to the drop point          -> expected
  * the same at an odd, overlapping offset (37, -23)          -> expected
  * a press/move/release drag whose source slot was painted
    faded while held, so only the press frame holds the card  -> expected
  * the object moved, but not to where the drag ended         -> not expected
  * a drag that changed nothing                               -> unexpected
  * the object vanished instead of moving                     -> not expected

and for every case that moved something, the recovered vector must be exact.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
VIEW_W, VIEW_H = 1280, 800
OBJ_W, OBJ_H = 240, 96


def textured_rect(img: np.ndarray) -> tuple[int, int]:
    """Top-left of the OBJ_W x OBJ_H window with the most textured 16px blocks."""
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    best, best_xy = -1, (0, 0)
    for y in range(32, VIEW_H // 2 - OBJ_H, 16):
        for x in range(32, VIEW_W // 2 - OBJ_W, 16):
            n = sum(gray[by:by + 16, bx:bx + 16].std() >= 4.0
                    for by in range(y, y + OBJ_H, 16) for bx in range(x, x + OBJ_W, 16))
            if n > best:
                best, best_xy = n, (x, y)
    return best_xy


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
        if src is None or src.shape[0] < VIEW_H or src.shape[1] < VIEW_W:
            print("stitched page too small for the test viewport", file=sys.stderr)
            return 2
        screen = src[:VIEW_H, :VIEW_W].copy()
        ax, ay = textured_rect(screen)
        obj = screen[ay:ay + OBJ_H, ax:ax + OBJ_W].copy()
        bg = np.median(screen.reshape(-1, 3), axis=0).astype(np.uint8)

        def moved(dx: int, dy: int) -> np.ndarray:
            out = screen.copy()
            out[ay:ay + OBJ_H, ax:ax + OBJ_W] = bg
            out[ay + dy:ay + dy + OBJ_H, ax + dx:ax + dx + OBJ_W] = obj
            return out

        vanished = screen.copy()
        vanished[ay:ay + OBJ_H, ax:ax + OBJ_W] = bg
        held = screen.copy()      # source slot painted faded while the card is held
        held[ay:ay + OBJ_H, ax:ax + OBJ_W] = (
            0.4 * obj + 0.6 * np.full_like(obj, 255)).astype(np.uint8)

        start = (ax + OBJ_W // 2, ay + OBJ_H // 2)
        far = (402, 141)
        cases = [
            # name, frames, actions [(kind, (x, y))], expected verdict of last step, mv
            ("carried to drop point", [screen, screen, moved(*far)],
             [("mouse_move", start), ("drag", (start[0] + far[0], start[1] + far[1]))],
             "expected", far),
            ("odd overlapping offset", [screen, screen, moved(37, -23)],
             [("mouse_move", start), ("drag", (start[0] + 37, start[1] - 23))],
             "expected", (37, -23)),
            ("press/move/release, faded source", [screen, held, held, moved(*far)],
             [("left_mouse_down", start),
              ("mouse_move", (start[0] + far[0], start[1] + far[1])),
              ("left_mouse_up", (start[0] + far[0], start[1] + far[1]))],
             "expected", far),
            ("moved, not to the drop point", [screen, screen, moved(*far)],
             [("mouse_move", start), ("drag", (start[0] - 200, start[1] + 400))],
             "not expected", far),
            ("changed nothing", [screen, screen, screen],
             [("mouse_move", start), ("drag", (start[0] + far[0], start[1] + far[1]))],
             "unexpected", None),
            ("vanished instead of moving", [screen, screen, vanished],
             [("mouse_move", start), ("drag", (start[0] + far[0], start[1] + far[1]))],
             "not expected", None),
        ]

        manifests = []
        for ci, (name, frames, actions, _, _) in enumerate(cases):
            d = tmp / f"case{ci}"
            d.mkdir()
            paths = []
            for fi, f in enumerate(frames):
                p = d / f"{fi:03d}.png"
                cv2.imwrite(str(p), f)
                paths.append(p)
            m = d / "manifest.tsv"
            with m.open("w") as fh:
                fh.write("# source=synthetic_drag\n")
                fh.write("index\taction\tx\ty\tdy\ttext\tbefore\tafter\n")
                for si, (kind, (x, y)) in enumerate(actions):
                    fh.write(f"{si}\t{kind}\t{x}\t{y}\t\t\t{paths[si]}\t{paths[si + 1]}\n")
            manifests.append(m)

        out = tmp / "drag.jsonl"
        subprocess.run([str(replay), *map(str, manifests), "-o", str(out), "--verify"],
                       check=True, capture_output=True)
        rows = [json.loads(l) for l in out.read_text().splitlines() if l.strip()]

    by_case: dict[str, dict] = {}
    for r in rows:
        by_case[r["trajectory"]] = r   # the last step of each case is the drag

    failures = []
    print(f"object {OBJ_W}x{OBJ_H} at ({ax},{ay})\n")
    print(f"{'case':<36}{'want':>14}{'verdict':>12}  {'mv':<12}reason")
    for ci, (name, _, _, want, mv) in enumerate(cases):
        r = by_case[f"case{ci}"]
        got = r["verdict"]
        clusters = r.get("drag_clusters", [])
        top = tuple(clusters[0]["mv"]) if clusters else None
        print(f"{name:<36}{want:>14}{got:>12}  {str(top):<12}{r['reason']}")
        ok = got != "expected" if want == "not expected" else got == want
        if not ok:
            failures.append(f"{name}: wanted {want}, got {got}")
        if mv is not None and top != tuple(mv):
            failures.append(f"{name}: wanted motion {mv}, recovered {top}")

    print()
    if failures:
        for f in failures:
            print(f"FAIL  {f}")
        return 1
    print(f"PASS  {len(cases)} drags judged correctly; every vector recovered exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
