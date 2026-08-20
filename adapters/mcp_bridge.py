#!/usr/bin/env python3
"""Normalize trycua/mcp-desktop-bridge trajectories into a Verty-CUA manifest.

    python3 adapters/mcp_bridge.py /path/to/mcp-desktop-bridge/trajectories -o out/

Layout produced by trycua's ComputerAgent (the bridge itself only logs the
replayable action subset; the screenshots come from the agent library):

    <run>/<agent-run>/turn_NNN/
        NNNN_screenshot_after.png
        NNNN_computer_call_result.json
        NNNN_screenshot_action.png

## The frame-ordering trap

The obvious reading -- pair `screenshot_after` with `screenshot_action` inside
one turn -- is wrong, and wrong in a way that silently produces a beautiful,
meaningless result.

`screenshot_action` is the SAME frame as `screenshot_after` with a cursor sprite
drawn at the action's coordinate. Measured across the corpus, that pair yields
changed_frac = 0.0003 with a bounding box of exactly 42x42 px centred on the
click, on every single step, regardless of what the action actually did. An M0
run built on it would report "100% tiny local change" and look like a triumph.

Both images in a turn are therefore the same moment. The question is WHICH
moment, and the file names invite the wrong answer twice over.

`screenshot_after` is the state **after that turn's own action**, despite being
written to disk before the `computer_call_result.json` that describes the action.
So the frames bracketing action i are turn i-1's and turn i's:

    turn_001  screenshot_after   <- state after action 1
              computer_call      <- action 1
              screenshot_action  <- same frame + cursor overlay (ignored)
    turn_002  (model turn: api_start / api_result / agent_response)
    turn_003  screenshot_after   <- state after action 2
              computer_call      <- action 2

    step for action 2:  turn_001.screenshot_after -> turn_003.screenshot_after

The FIRST action is dropped: nothing captured the state before it.

This was originally implemented the other way round -- pairing turn i with turn
i+1 and attributing turn i's action -- which shifted every step by one, so each
action was scored against the change produced by the *next* one. It survived
review because the numbers stayed plausible: changes were still local, still
near *a* click, just the wrong one.

It was caught by asking which coordinate the change actually sits closer to,
over the whole corpus:

    | corpus          | dist to this action | dist to next action | verdict     |
    | mcp_bridge      | 14.8 px median      | 10.5 px median      | off by one  |
    | qwen_cua_demo   | 22.0 px median      | 109.7 px median     | correct     |

The demo adapter, whose pairing follows from explicit event semantics rather
than from file names, is the control that shows the test discriminates.

## Corpus caveat

These recordings are exploratory sessions on a collaborative office suite, not a
designed corpus. There is no scrolling in them at all, and the interaction is
mostly one small context menu -- so they exercise the classifier's local-change
and no-change paths and say nothing about its scroll path. Their real value is
the clicks that landed in empty space and changed nothing, which are exactly the
turns where a VLM call was spent to observe that nothing happened.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

TURN_RE = re.compile(r"turn_(\d+)$")

# trycua action vocabulary -> the normalized one used by the manifest.
ACTION_MAP = {
    "click": "click",
    "double_click": "double_click",
    "right_click": "right_click",
    "left_click": "click",
    "type": "type",
    "keypress": "key",
    "key": "key",
    "scroll": "scroll",
    "drag": "drag",
    "move": "mouse_move",
    "wait": "wait",
    "screenshot": "screenshot",
}


def read_action(path: Path) -> dict | None:
    try:
        doc = json.loads(path.read_text())
    except Exception:
        return None
    item = doc.get("item") or {}
    action = item.get("action")
    return action if isinstance(action, dict) else None


def collect_turns(agent_dir: Path) -> list[dict]:
    """Action turns in order, each with its before-frame and its action."""
    turns = []
    for d in sorted(agent_dir.iterdir()):
        if not d.is_dir() or not TURN_RE.search(d.name):
            continue
        after = sorted(d.glob("*_screenshot_after.png"))
        call = sorted(d.glob("*_computer_call_result.json"))
        if not after or not call:
            continue  # model turn, or an incomplete one
        action = read_action(call[0])
        if action is None:
            continue
        turns.append({"dir": d, "frame": after[0], "action": action})
    return turns


def normalize(action: dict) -> dict:
    raw = str(action.get("type", "")).strip()
    kind = ACTION_MAP.get(raw, raw or "unknown")
    out = {"action": kind, "x": "", "y": "", "dy": "", "text": ""}

    x, y = action.get("x"), action.get("y")
    if isinstance(x, (int, float)) and isinstance(y, (int, float)):
        out["x"], out["y"] = int(x), int(y)

    # trycua spells scroll deltas several ways depending on the adapter version.
    for key in ("scroll_y", "delta_y", "dy", "amount"):
        v = action.get(key)
        if isinstance(v, (int, float)):
            out["dy"] = int(v)
            break

    text = action.get("text")
    if text is None:
        keys = action.get("keys")
        if isinstance(keys, list):
            text = "+".join(str(k) for k in keys)
        elif isinstance(keys, str):
            text = keys
    if isinstance(text, str):
        # Tabs and newlines would break the TSV; the exact typed bytes are not
        # what M0 measures.
        out["text"] = text.replace("\t", " ").replace("\n", " ").replace("\r", " ")
    return out


def write_manifest(agent_dir: Path, out_dir: Path) -> tuple[Path, int] | None:
    turns = collect_turns(agent_dir)
    if len(turns) < 2:
        return None

    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = out_dir / "manifest.tsv"
    rows = 0
    with manifest.open("w") as fh:
        fh.write("# source=mcp_bridge\n")
        fh.write(f"# agent_run={agent_dir.name}\n")
        # turns[i]["frame"] is the state AFTER turns[i]["action"], so action i is
        # bracketed by frames i-1 and i. Nothing captured the state before the
        # first action, so it cannot be a step.
        fh.write(f"# steps={len(turns) - 1} (first action dropped: no frame before it)\n")
        fh.write("index\taction\tx\ty\tdy\ttext\tbefore\tafter\n")
        for i in range(1, len(turns)):
            a = normalize(turns[i]["action"])
            fh.write("\t".join([
                str(i - 1), a["action"], str(a["x"]), str(a["y"]), str(a["dy"]),
                a["text"],
                str(turns[i - 1]["frame"].resolve()),
                str(turns[i]["frame"].resolve()),
            ]) + "\n")
            rows += 1
    return manifest, rows


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("root", help="mcp-desktop-bridge/trajectories directory")
    p.add_argument("-o", "--out", default="trajectories",
                   help="where to write per-run manifests (default ./trajectories)")
    args = p.parse_args()

    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1
    out_root = Path(args.out).expanduser().resolve()

    total_steps = 0
    written = 0
    for run in sorted(root.iterdir()):
        if not run.is_dir():
            continue
        for agent_dir in sorted(run.iterdir()):
            if not agent_dir.is_dir():
                continue
            res = write_manifest(agent_dir, out_root / run.name / agent_dir.name)
            if res is None:
                continue
            manifest, rows = res
            written += 1
            total_steps += rows
            print(f"{rows:4d} steps  {manifest}")

    print(f"\n{written} manifests, {total_steps} usable steps -> {out_root}")
    if not written:
        print("no usable trajectories found -- expected <root>/<run>/<agent-run>/turn_NNN/",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
