#!/usr/bin/env python3
"""Render before/after contact sheets for verified steps, and score them by hand.

    python3 review_sheet.py m0_all.jsonl -o review/         # render the pool
    python3 review_sheet.py m0_all.jsonl --score review/    # confusion matrix

Each step becomes one PNG: the before frame, the after frame, and a change map
coloured by history (green = content never seen before, red = an earlier
appearance returned), with the action drawn on it and the verifier's verdict in
the caption.

## Labelling

Move each sheet into the folder naming what the verdict SHOULD have been:

    review/expected/     the action did what it should have
    review/unexpected/   the action failed, or the result contradicts it
    review/unknown/      genuinely ambiguous even to a human with both frames

The label records your judgement of the truth, not a yes/no on the suggestion.
That distinction carries the whole point: a step predicted `unknown` that is
really `expected` is not merely a wrong answer, it is a VLM call the rules could
have saved and did not -- and it is a completely different problem from a step
predicted `expected` that is really `unexpected`, which is a silent task
failure. Collapsing both into "wrong" hides which one you have.

Scoring reads the prediction from the filename and the truth from the folder,
and reports the confusion matrix plus the two numbers that matter:

    precision on `expected`   safety   -- gates M3, bar is 95%
    recall on `expected`      headroom -- how much saving is being left behind
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

FONT = cv2.FONT_HERSHEY_SIMPLEX
BAR_H = 74
GAP = 8


def panel(img_bgr, title, width, height):
    """Letterbox one frame into a fixed cell with a caption strip."""
    h, w = img_bgr.shape[:2]
    scale = min(width / w, height / h)
    resized = cv2.resize(img_bgr, (max(1, int(w * scale)), max(1, int(h * scale))),
                         interpolation=cv2.INTER_AREA)
    cell = np.full((height, width, 3), 32, np.uint8)
    y0 = (height - resized.shape[0]) // 2
    x0 = (width - resized.shape[1]) // 2
    cell[y0:y0 + resized.shape[0], x0:x0 + resized.shape[1]] = resized
    cv2.putText(cell, title, (8, 22), FONT, 0.6, (230, 230, 230), 1, cv2.LINE_AA)
    return cell, scale, x0, y0


def build_sheet(row, before_path, after_path, cell_w=680, cell_h=470,
                change_map=None):
    before = cv2.imread(str(before_path))
    after = cv2.imread(str(after_path))
    if before is None or after is None or before.shape != after.shape:
        return None

    # Difference map over a dimmed copy of the after frame, which is what makes
    # "nothing happened" instantly readable.
    #
    # With a change map from verty-replay the change is split by its HISTORY
    # rather than shown as undifferentiated red:
    #
    #   green  NOVEL    this block has never looked like this before
    #   red    REVISIT  an earlier appearance of this block came back -- a menu
    #                   closed, a dialog dismissed, an edit undone, a hover ended
    #
    # A binary mask cannot tell "a dropdown opened" from "the dropdown closed
    # again"; both are just change. The distinction needs no model, only the
    # frames already on disk.
    diff = cv2.absdiff(cv2.cvtColor(before, cv2.COLOR_BGR2GRAY),
                       cv2.cvtColor(after, cv2.COLOR_BGR2GRAY))
    changed = (cv2.blur(diff, (3, 3)) > 18).astype(np.uint8)
    # Backdrop for the change map: a dimmed BLEND of both frames, not the after
    # frame alone.
    #
    # Drawing over `after` privileges one layout. When content reflows or moves
    # between frames, marks belonging to the before-layout -- the outline of a
    # page edge that has since shifted, say -- land over after-space where
    # nothing is, and read as highlighting empty background. Blending ghosts
    # both positions, so a mark always has something under it that explains it.
    heat = (cv2.addWeighted(before, 0.5, after, 0.5, 0) * 0.35).astype(np.uint8)

    labels = None
    if change_map is not None and Path(change_map).is_file():
        labels = cv2.imread(str(change_map), cv2.IMREAD_GRAYSCALE)
        if labels is not None and labels.shape != changed.shape:
            labels = cv2.resize(labels, (changed.shape[1], changed.shape[0]),
                                interpolation=cv2.INTER_NEAREST)
    if labels is None:
        heat[changed > 0] = (60, 60, 255)
    else:
        # The block grid is coarser than the pixel mask, so intersect: colour
        # only pixels that actually changed, using their block's class.
        heat[(changed > 0) & (labels == 1)] = (90, 215, 90)    # novel  -> green
        heat[(changed > 0) & (labels == 2)] = (60, 60, 255)    # revisit-> red
        heat[(changed > 0) & (labels == 0)] = (60, 60, 255)    # unclassified

    cells = []
    for img, title in ((before, "BEFORE"), (after, "AFTER"),
                       (heat, "CHANGED  (over both frames blended)")):
        cell, scale, ox, oy = panel(img, title, cell_w, cell_h)
        if "x" in row:
            # Draw the action point in each cell, in that cell's own coordinates.
            cx = int(row["x"] * scale) + ox
            cy = int(row["y"] * scale) + oy
            cv2.drawMarker(cell, (cx, cy), (0, 220, 255), cv2.MARKER_CROSS, 22, 2)
            cv2.circle(cell, (cx, cy), 14, (0, 220, 255), 1, cv2.LINE_AA)
        cells.append(cell)

    strip = np.full((cell_h, GAP, 3), 32, np.uint8)
    body = np.hstack([cells[0], strip, cells[1], strip, cells[2]])

    bar = np.full((BAR_H, body.shape[1], 3), 20, np.uint8)
    verdict = row.get("verdict", "-")
    colour = {"expected": (110, 220, 110), "unexpected": (110, 110, 240),
              "unknown": (180, 180, 180)}.get(verdict, (200, 200, 200))
    head = f"{row['action']}"
    if "x" in row:
        head += f" @ ({row['x']},{row['y']})"
    if row.get("text"):
        head += f'  text="{row["text"][:28]}"'
    cv2.putText(bar, head, (10, 26), FONT, 0.62, (235, 235, 235), 1, cv2.LINE_AA)
    cv2.putText(bar, verdict.upper(), (10, 54), FONT, 0.68, colour, 2, cv2.LINE_AA)
    meta = (f"changed={row['changed_frac']:.4f}  extent={row.get('extent','-')}  "
            f"comps={row['components']}  transition={row['transition']}"
            + (f"  dy={row['dy']}" if row["transition"] == "scroll" else ""))
    cv2.putText(bar, meta, (200, 54), FONT, 0.5, (185, 185, 185), 1, cv2.LINE_AA)
    if row.get("blocks_changed"):
        x = body.shape[1] - 320
        cv2.putText(bar, "novel", (x, 26), FONT, 0.46, (90, 215, 90), 1, cv2.LINE_AA)
        cv2.putText(bar, f"{row.get('blocks_novel', 0)}", (x + 58, 26), FONT, 0.46,
                    (235, 235, 235), 1, cv2.LINE_AA)
        cv2.putText(bar, "revisit", (x + 110, 26), FONT, 0.46, (90, 90, 245), 1, cv2.LINE_AA)
        cv2.putText(bar, f"{row.get('blocks_revisit', 0)}", (x + 178, 26), FONT, 0.46,
                    (235, 235, 235), 1, cv2.LINE_AA)
        cv2.putText(bar, "blocks", (x + 232, 26), FONT, 0.42, (150, 150, 150), 1, cv2.LINE_AA)
    reason = row.get("reason", "")
    if reason:
        cv2.putText(bar, reason[:110], (10, BAR_H - 6), FONT, 0.44,
                    (150, 150, 150), 1, cv2.LINE_AA)
    return np.vstack([bar, body])


def manifest_frames(manifest_root: Path) -> dict[tuple[str, int], tuple[str, str]]:
    """(trajectory, step) -> (before, after), read back from the manifests."""
    out = {}
    for m in manifest_root.rglob("manifest.tsv"):
        traj = m.parent.name
        cols = None
        for line in m.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            cells = line.split("\t")
            if cols is None:
                cols = cells
                continue
            row = dict(zip(cols, cells))
            out[(traj, int(row["index"]))] = (row["before"], row["after"])
    return out


def parse_sheet_name(name: str):
    """`<verdict>__<traj>__<step>_<action>[_<x>x<y>].png` -> (verdict, traj, tail).

    `tail` is everything identifying WHAT was judged -- step, action and
    coordinate -- so a label only survives while the sheet would be rendered
    identically.
    """
    stem = name[:-4] if name.endswith(".png") else name
    parts = stem.split("__")
    if len(parts) < 3:
        return None
    return parts[0], parts[1], parts[2]


VERDICTS = ("expected", "unexpected", "unknown")


def score(review_dir: Path, jsonl: str | None) -> None:
    """Confusion matrix of predicted verdict against hand-labelled truth."""
    current: dict[tuple[str, str], str] = {}
    if jsonl and Path(jsonl).is_file():
        for line in Path(jsonl).read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                if "verdict" in r:
                    loc = f"_{r['x']}x{r['y']}" if "x" in r else ""
                    current[(r["trajectory"], f"{r['step']:03d}_{r['action']}{loc}")] = r["verdict"]

    matrix = {p: {t: 0 for t in VERDICTS} for p in VERDICTS}
    stale: list[tuple[str, str, str]] = []
    orphans: list[str] = []
    n = 0
    for truth in VERDICTS:
        for p in (review_dir / truth).glob("*.png"):
            parsed = parse_sheet_name(p.name)
            if parsed is None:
                continue
            predicted, traj, tail = parsed
            now = current.get((traj, tail))
            if now is None:
                # No such step in this run at all: the sheet predates an adapter
                # or rule change that renamed what it showed. Counting it would
                # score a judgement about a picture that no longer exists.
                orphans.append(p.name)
                continue
            if now != predicted:
                stale.append((p.name, predicted, now))
                continue
            if predicted in matrix:
                matrix[predicted][truth] += 1
                n += 1

    if not n and not stale:
        print(f"no labels yet. Move sheets into {review_dir}/expected, "
              f"{review_dir}/unexpected or {review_dir}/unknown.")
        return

    if n:
        print(f"{n} labelled steps\n")
        print(f"{'':>14}{'TRUTH':^36}")
        print(f"{'predicted':>14}" + "".join(f"{t:>12}" for t in VERDICTS))
        for pred in VERDICTS:
            row = matrix[pred]
            print(f"{pred:>14}" + "".join(f"{row[t]:>12}" for t in VERDICTS))

        exp = matrix["expected"]
        tp = exp["expected"]
        prec_n = sum(exp.values())
        rec_n = sum(matrix[p]["expected"] for p in VERDICTS)
        print()
        if prec_n:
            print(f"  precision on `expected`  {tp}/{prec_n} = {100.0 * tp / prec_n:.1f}%"
                  f"   (safety -- bar is 95%)")
        if rec_n:
            print(f"  recall on `expected`     {tp}/{rec_n} = {100.0 * tp / rec_n:.1f}%"
                  f"   (headroom -- savings left behind)")

        danger = exp["unexpected"]
        missed = matrix["unknown"]["expected"]
        if danger:
            print(f"\n  !! {danger} step(s) predicted `expected` that actually failed."
                  f" Each is a silent task failure.")
        if missed:
            print(f"  {missed} step(s) predicted `unknown` that were really `expected`"
                  f" -- VLM calls the rules could have saved.")
        over = matrix["unexpected"]["expected"]
        if over:
            print(f"  {over} step(s) predicted `unexpected` that were fine"
                  f" -- guard mode would abort these wrongly.")

    if orphans:
        print(f"\n{len(orphans)} label(s) orphaned -- no matching step in this run "
              f"(pre-date an adapter or rule change); move them out or re-review:")
        for name in orphans[:6]:
            print(f"  {name}")
        if len(orphans) > 6:
            print(f"  ... and {len(orphans) - 6} more")

    if stale:
        print(f"\n{len(stale)} label(s) stale -- what the sheet showed has changed "
              f"since labelling, so they need re-review:")
        for name, was, now in stale:
            print(f"  {was} -> {now}   {name}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jsonl", nargs="?", help="verty-replay output (run with --verify)")
    ap.add_argument("-o", "--out", default="review")
    ap.add_argument("-m", "--manifests", default="trajectories")
    ap.add_argument("-c", "--change-maps", default="changemaps",
                    help="dir written by verty-replay --change-maps; enables the "
                         "green/red novel-vs-revisit colouring")
    ap.add_argument("--verdict", help="only render this verdict class")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--cell", type=int, default=680,
                    help="width of each panel in px (default 680)")
    ap.add_argument("--score", metavar="DIR", help="score a labelled review directory")
    args = ap.parse_args()

    if args.score:
        score(Path(args.score), args.jsonl)
        return 0
    if not args.jsonl:
        ap.error("jsonl is required unless --score is given")

    rows = [json.loads(l) for l in Path(args.jsonl).read_text().splitlines() if l.strip()]
    if args.verdict:
        rows = [r for r in rows if r.get("verdict") == args.verdict]
    if args.limit:
        rows = rows[:args.limit]
    if not rows:
        print("nothing to render", file=sys.stderr)
        return 1

    frames = manifest_frames(Path(args.manifests))
    out = Path(args.out)
    for sub in ("", *VERDICTS):
        (out / sub).mkdir(parents=True, exist_ok=True)

    # Sheets already sorted into yes/ or no/ are labelled work. Re-rendering
    # them into the root pool would silently ask for the same judgement twice,
    # and a verdict that changed since labelling would appear under a NEW name
    # while the old one sat in yes/ -- two sheets for one step, disagreeing.
    # A step counts as labelled only while its verdict still matches the one that
    # was judged; if a rule changed it, the judgement no longer applies and the
    # step goes back into the pool.
    labelled_steps = set()
    for sub in VERDICTS:
        for p in (out / sub).glob("*.png"):
            parsed = parse_sheet_name(p.name)
            if parsed:
                labelled_steps.add((parsed[1], parsed[2], parsed[0]))  # traj, tail, verdict

    made = missing = relabelled = 0
    for r in rows:
        loc_ = f"_{r['x']}x{r['y']}" if "x" in r else ""
        tail_ = f"{r['step']:03d}_{r['action']}{loc_}"
        if (r["trajectory"], tail_, r.get("verdict")) in labelled_steps:
            relabelled += 1
            continue
        key = (r["trajectory"], r["step"])
        if key not in frames:
            missing += 1
            continue
        cmap = Path(args.change_maps) / r["trajectory"] / f"{r['step']:03d}.png"
        sheet = build_sheet(r, *frames[key], cell_w=args.cell,
                            cell_h=int(args.cell * 0.69), change_map=cmap)
        if sheet is None:
            missing += 1
            continue
        # The name carries verdict, step, action AND coordinate. A label answers
        # "is this verdict right for THIS picture with THIS action on it", so any
        # of those changing must invalidate the label. Leaving the coordinate out
        # let an adapter fix that re-attributed every step slip past staleness
        # detection: the verdict and step matched, the action often matched too,
        # and eight labels silently became judgements of a different action.
        loc = f"_{r['x']}x{r['y']}" if "x" in r else ""
        name = (f"{r.get('verdict','none')}__{r['trajectory']}"
                f"__{r['step']:03d}_{r['action']}{loc}.png")
        cv2.imwrite(str(out / name), sheet)
        made += 1

    print(f"{made} sheets -> {out}/   ({missing} skipped: frames not found, "
          f"{relabelled} already labelled)")
    print(f"\nLabel by moving each sheet into the folder naming what the verdict")
    print(f"SHOULD have been -- {out}/expected, {out}/unexpected or {out}/unknown")
    print(f"-- then:\n  python3 review_sheet.py {args.jsonl} --score {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
