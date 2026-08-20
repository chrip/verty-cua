#!/usr/bin/env python3
"""Normalize Qwen-CUA demo runs into a Verty-CUA manifest.

    python3 adapters/qwen_cua_demo.py /path/to/Qwen-CUA/demo/data/runs -o trajectories/

Each run directory holds:

    run.json  events.jsonl  replay.json  screenshots/NNNN.png  downloads/  uploads/

Pairing is straightforward here, unlike the trycua bridge format: the runner
captures a screenshot immediately after every executed action and emits an
`action_completed` event naming it. So walking events in sequence order and
tracking the last screenshot seen gives (S_before, action, S_after) directly.

Two conversions matter:

* **Coordinates are relative, not pixels.** The demo's protocol puts the model
  on a 0..999 grid (`actions.py` validates it), while Verty works in frame
  pixels. The screenshot artifact carries the real width and height, so the
  conversion is exact rather than assumed.
* **Scroll signs are inverted, and not for the obvious reason.** Verty's `dy` is
  positive when the page scrolls DOWN (`scroll_detector.hpp`; confirmed on a
  recorded session where `dy=+400` steps carry a viewport top of 400, 800,
  1200...). The demo executes `scroll` as `page.mouse.wheel(0, -action.pixels)`
  (`computer.py`) -- it negates the delta, so a POSITIVE `pixels` scrolls the
  view UP. Hence `dy = -pixels`.

  This is worth stating precisely because the scroll path is the one part of the
  verifier no recorded corpus has exercised yet: a sign error here would make
  every scroll verify as "scrolled against the requested direction" and look
  like a real finding.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# The demo's action vocabulary is already close to the normalized one; only the
# names that differ are listed.
ACTION_MAP = {
    "left_click": "click",
    "left_click_drag": "drag",
    "mouse_move": "mouse_move",
    "key_down": "key",
    "key_up": "key",
    # Kept distinct from `drag`: a hand-rolled press/move/release sequence has
    # different expected geometry at each step than a single left_click_drag.
    "left_mouse_down": "left_mouse_down",
    "left_mouse_up": "left_mouse_up",
}

# Actions that never change the screen by themselves are still recorded: a
# `screenshot` or `wait` step that shows a change is evidence of an async update,
# which is exactly the case a naive verifier gets wrong.
SKIP_ACTIONS = {"terminate", "call_user"}


def load_replay(run_dir: Path) -> dict | None:
    replay = run_dir / "replay.json"
    if replay.is_file():
        try:
            return json.loads(replay.read_text())
        except Exception:
            return None
    # A run killed mid-flight never writes replay.json, but events.jsonl is
    # appended as it goes and is still usable.
    events = run_dir / "events.jsonl"
    if not events.is_file():
        return None
    rows = []
    for line in events.read_text().splitlines():
        if line.strip():
            try:
                rows.append(json.loads(line))
            except Exception:
                pass
    run = {}
    if (run_dir / "run.json").is_file():
        try:
            run = json.loads((run_dir / "run.json").read_text())
        except Exception:
            pass
    return {"run": run, "events": rows,
            "artifacts": {"screenshots": run.get("screenshots", [])}}


def screenshot_index(replay: dict) -> dict[str, dict]:
    shots = (replay.get("artifacts") or {}).get("screenshots") or []
    if not shots:
        shots = (replay.get("run") or {}).get("screenshots") or []
    return {s["id"]: s for s in shots if isinstance(s, dict) and "id" in s}


def frame_path(run_dir: Path, shot: dict) -> Path | None:
    seq = shot.get("sequence")
    if seq is None:
        return None
    p = run_dir / "screenshots" / f"{int(seq):04d}.png"
    return p if p.is_file() else None


def normalize(detail: dict, shot: dict) -> dict | None:
    raw = str(detail.get("action", "")).strip()
    if not raw or raw in SKIP_ACTIONS:
        return None
    out = {"action": ACTION_MAP.get(raw, raw), "x": "", "y": "", "dy": "", "text": ""}

    coord = detail.get("coordinate")
    if isinstance(coord, (list, tuple)) and len(coord) == 2:
        w, h = shot.get("width"), shot.get("height")
        if isinstance(w, int) and isinstance(h, int) and w > 0 and h > 0:
            # 0..999 relative grid -> frame pixels.
            out["x"] = int(round(float(coord[0]) / 1000.0 * w))
            out["y"] = int(round(float(coord[1]) / 1000.0 * h))

    pixels = detail.get("pixels")
    if isinstance(pixels, (int, float)) and raw == "scroll":
        # computer.py: wheel(0, -pixels), so positive pixels scrolls the view UP.
        # Verty: dy > 0 means the page scrolled DOWN. See the module docstring.
        out["dy"] = -int(pixels)

    text = detail.get("text")
    if text is None and isinstance(detail.get("keys"), list):
        text = "+".join(str(k) for k in detail["keys"])
    if isinstance(text, str):
        out["text"] = text.replace("\t", " ").replace("\n", " ").replace("\r", " ")
    return out


def write_manifest(run_dir: Path, out_dir: Path) -> tuple[Path, int] | None:
    replay = load_replay(run_dir)
    if not replay:
        return None
    shots = screenshot_index(replay)
    events = sorted(
        (e for e in replay.get("events", []) if isinstance(e, dict)),
        key=lambda e: e.get("sequence", 0),
    )

    # The before-frame is the screenshot left behind by the PREVIOUS action, not
    # simply the last screenshot seen. The runner emits `screenshot_captured` and
    # then `action_completed` referring to the SAME screenshot, so tracking every
    # event that carries a screenshot id makes the before-frame equal the
    # after-frame and every step measures a frame against itself -- silently, as
    # changed_frac 0.0 on every row.
    steps = []
    prev_post: str | None = None      # screenshot after the previous action
    for ev in events:
        sid = ev.get("screenshot_id")
        etype = ev.get("type")

        # Seed with the observation captured before any action ran.
        if prev_post is None and etype == "screenshot_captured" and sid:
            prev_post = sid
            continue

        if etype == "action_completed" and sid and prev_post and sid != prev_post:
            before, after = shots.get(prev_post), shots.get(sid)
            if before and after:
                bp, ap = frame_path(run_dir, before), frame_path(run_dir, after)
                detail = ev.get("detail")
                if bp and ap and isinstance(detail, dict):
                    norm = normalize(detail, before)
                    if norm:
                        steps.append((norm, bp, ap))
            prev_post = sid

    if not steps:
        return None

    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = out_dir / "manifest.tsv"
    run = replay.get("run") or {}
    with manifest.open("w") as fh:
        fh.write("# source=qwen_cua_demo\n")
        fh.write(f"# run={run.get('id', run_dir.name)}\n")
        if run.get("scenario_id"):
            fh.write(f"# scenario={run['scenario_id']}\n")
        if run.get("model"):
            fh.write(f"# model={run['model']}\n")
        summary = run.get("summary") or {}
        if summary:
            fh.write(f"# outcome={summary.get('outcome')} "
                     f"verification={summary.get('verification')}\n")
        fh.write("index\taction\tx\ty\tdy\ttext\tbefore\tafter\n")
        for i, (a, bp, ap) in enumerate(steps):
            fh.write("\t".join([
                str(i), a["action"], str(a["x"]), str(a["y"]), str(a["dy"]),
                a["text"], str(bp.resolve()), str(ap.resolve()),
            ]) + "\n")
    return manifest, len(steps)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="Qwen-CUA demo data/runs directory")
    ap.add_argument("-o", "--out", default="trajectories")
    args = ap.parse_args()

    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1
    out_root = Path(args.out).expanduser().resolve()

    total = written = 0
    for run_dir in sorted(root.iterdir()):
        if not run_dir.is_dir():
            continue
        res = write_manifest(run_dir, out_root / "qwen_cua_demo" / run_dir.name)
        if res is None:
            continue
        manifest, rows = res
        written += 1
        total += rows
        print(f"{rows:4d} steps  {manifest}")

    print(f"\n{written} manifests, {total} usable steps -> {out_root}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
