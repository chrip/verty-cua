#!/usr/bin/env python3
"""
test_stitch_session.py — regression benchmark for the stitching algorithm.

Runs `verty` on a recorded session and scores the tool's session
report (-r JSON) against that session's ground_truth.json (measured in the
real browser). Every run is appended to stitch_test_results.jsonl so the
metrics stay comparable as the algorithm improves — lower `totalError` is
better. History comparisons are per session.

Session types (auto-detected from the ground-truth schema):

  single     (e.g. session_page1)  — stitch metrics (px):
      width, height    reconstructed document size vs. viewport / pageHeight
      header, footer,  detected fixed bands vs. ground truth
      left, right
    plus scroll-event metrics (Milestone 2):
      dyErrorSum       Σ |dy_algo − dy_gt| over the union of scrolling frames
      eventCountDiff   |#events_algo − #events_gt|
      pageChanges      PAGE_CHANGE transitions (must be 0: false-positive guard)

  multipage  (e.g. session_multipage) — segmentation metrics (Milestone 3):
      pageCount        detected pages vs. ground truth
      boundaryErr      |first stable frame per navigation − ground truth|
      per-page width / docHeight vs. what the visit actually saw
      (fixed bands are only scored for visits that scrolled enough to
       reveal them — a no-scroll visit cannot detect fixed elements)
    plus the same scroll-event scoring across the whole session.

Usage:
    python3 test_stitch_session.py [sessionDir] [--strict]
    python3 test_stitch_session.py --all [--strict]
        sessionDir  session folder (default: session_page1, next to this file)
        --all       run every session_*/ that has a ground_truth.json and
                    print a summary — one command scores the whole contract
        --strict    exit non-zero if any metric is outside tolerance
"""
import os, sys, json, subprocess
from datetime import datetime, timezone

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BINARY = os.path.join(REPO, "build", "verty")
RESULTS_LOG = os.path.join(HERE, "stitch_test_results.jsonl")

# Stitch tolerances (px). Band tolerances match the contract's declared
# uncertainty (FIXED_BAND_UNCERTAINTY_PX = 48): bands only ever OVER-detect —
# flat margins read as stable — and the margin is invisible in the output
# (documented non-decision). totalError still tracks the raw errors, so
# improvements remain visible in the history.
TOLERANCE = {"width": 0, "height": 32, "header": 48, "footer": 48, "left": 48, "right": 48}

# Scroll-event tolerances (Milestone 2). Baseline measured 2026-07-03:
# dyErrorSum=6 and eventCountDiff=6 — all six misses are |dy|=1 steps, which the
# ScrollDetector rejects as noise BY DESIGN (|dy|≤1 gate); they also account for
# the stitch's -2px height drift. pageChanges=0. Tolerances = baseline + margin.
SCROLL_TOLERANCE = {"dyErrorSum": 12, "eventCountDiff": 8, "pageChanges": 0}

# Multipage tolerances (Milestone 3). Baseline measured 2026-07-03 (totalError
# 82px): boundaryErr=0 (the fixture's hard 1-frame cut detected exactly),
# page-0 height -2 / page-1 height 0; fixed bands on the short 282px visit:
# header -1, footer -1, left +39, right +39 (sidebar over-detection as on the
# full session — documented non-decision); page-1 bands n/a (no scrolling).
# Milestone 4 adds navK.type (NAVIGATE|REVISIT, exact) and navK.vtStart for
# revisits (the entry scroll offset recovered by the page identifier).
MULTIPAGE_TOLERANCE = {
    "boundaryFrames": 3,     # per navigation, roadmap M3 exit criterion
    "widthPx": 0,
    "heightPx": 32,          # per page, vs what the visit actually saw
    "fixedPx": 48,           # per band, only for visits that scrolled ≥ minScrollForFixed
    "minScrollForFixed": 150,
    "vtStartPx": 32,         # revisit entry offset vs ground truth
}


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


def git_info():
    def run(*a):
        try:
            return subprocess.check_output(a, cwd=REPO, text=True,
                                           stderr=subprocess.DEVNULL).strip()
        except Exception:
            return ""
    return {"commit": run("git", "rev-parse", "--short", "HEAD"),
            "dirty": bool(run("git", "status", "--porcelain"))}


def fixed_from_rects(rects):
    """fixedRects list → {header,footer,left,right} band extents in px."""
    out = {"header": 0, "footer": 0, "left": 0, "right": 0}
    for r in rects:
        if r["kind"] in ("header", "footer"):
            out[r["kind"]] = r["h"]
        elif r["kind"] in ("left", "right"):
            out[r["kind"]] = r["w"]
    return out


# ── Scroll-event scoring (shared) ─────────────────────────────────────────────
def gt_scroll_events(gt):
    """Per-frame ground-truth dy map. Frames spanning a page change or a
    transition frame (page None) carry no scroll expectation."""
    fr = gt["frames"]
    ev = {}
    for i in range(1, len(fr)):
        a, b = fr[i - 1], fr[i]
        if a.get("page", 0) is None or b.get("page", 0) is None:
            continue
        if a.get("page", 0) != b.get("page", 0):
            continue
        d = round(b["scrollY"] - a["scrollY"])
        if d:
            ev[i] = d
    return ev


# Ghost-score tolerances (Milestone 5): mean 16×16-block MAD between the
# stitch interior and the browser's chrome-hidden full-page render
# (fullpage.png), locally aligned per block-row (drift is not ghosting),
# animated regions excluded. Baseline measured 2026-07-04 on session_hostile:
# madMean=3.84, badBlockFrac=1.13% (floating-button remnants). Tolerances =
# baseline + margin.
GHOST_TOLERANCE = {"madMean": 6.0, "badBlockFrac": 0.02}


def score_ghost(gt, session, stitch_png):
    """Perceptual ghosting score vs the browser's full-page reference render.
    Returns None when the session has no fullpage.png."""
    ref_path = os.path.join(session, "fullpage.png")
    if not os.path.exists(ref_path):
        return None
    ref = cv2.imread(ref_path)
    stitch = cv2.imread(stitch_png)
    if ref is None or stitch is None:
        return None

    vw, vh = gt["viewport"]["width"], gt["viewport"]["height"]
    f = gt["fixed"]
    x0, x1 = int(f["leftWidth"]), vw - int(f["rightWidth"])
    y0 = int(f["headerHeight"]) + 8
    y1 = min(ref.shape[0], stitch.shape[0]) - int(f["footerHeight"]) - 8

    # Valid-pixel mask in document coordinates (True = scored). The reference
    # is the pure scrolling document (chrome hidden at capture), so only the
    # content interior is compared — chrome columns/rows are cropped away.
    valid = np.zeros((y1, x1), dtype=bool)
    valid[y0:y1, x0:x1] = True
    # Animated regions are legitimately different (another animation instant);
    # conditionally-fixed overlays are stamped once, anchored to their viewport
    # edge — the chrome-hidden reference has neither.
    for d in gt.get("dynamicRegions", []):
        if d.get("kind") == "animated":
            b = d["doc"]
            ya, yb = max(0, int(b["y"]) - 8), min(y1, int(b["y"] + b["height"]) + 8)
            xa, xb = max(0, int(b["x"]) - 8), min(x1, int(b["x"] + b["width"]) + 8)
            valid[ya:yb, xa:xb] = False
        elif d.get("kind") == "conditional-fixed":
            b = d["viewport"]
            bottom = b["y"] + b["height"] / 2 > vh / 2
            doc_y = (stitch.shape[0] - (vh - int(b["y"]))) if bottom else int(b["y"])
            ya, yb = max(0, doc_y - 8), min(y1, doc_y + int(b["height"]) + 8)
            xa, xb = max(0, int(b["x"]) - 8), min(x1, int(b["x"] + b["width"]) + 8)
            valid[ya:yb, xa:xb] = False

    g_ref    = cv2.cvtColor(ref[:y1, :x1],    cv2.COLOR_BGR2GRAY).astype(np.int16)
    g_stitch = cv2.cvtColor(stitch[:y1, :x1], cv2.COLOR_BGR2GRAY).astype(np.int16)

    # Per-block MAD with LOCAL vertical alignment: scroll-accumulation drift
    # grows along the page (measured ~8px at the bottom) and is visually
    # invisible, so each 16px block-row is scored at its own best shift within
    # ±12px. Structural ghosting (smears, torn/mixed frames) cannot be shifted
    # away and still scores high.
    B, S = 16, 12
    H, W = (y1 // B) * B, (x1 // B) * B
    per_shift = []
    for shift in range(-S, S + 1):
        r = np.roll(g_ref, shift, axis=0)
        diff = np.abs(r - g_stitch).astype(np.float64)
        diff[~valid] = np.nan
        blocks = diff[:H, :W].reshape(H // B, B, W // B, B).transpose(0, 2, 1, 3)
        with np.errstate(invalid="ignore"):
            per_shift.append(np.nanmean(blocks.reshape(H // B, W // B, -1), axis=2))
    stack = np.stack(per_shift)                      # (shifts, rows, cols)
    with np.errstate(invalid="ignore"):
        row_mean = np.nanmean(stack, axis=2)         # (shifts, rows)
    row_mean[np.isnan(row_mean)] = np.inf
    best_shift = row_mean.argmin(axis=0)             # per block-row
    bmad = np.take_along_axis(stack, best_shift[None, :, None], axis=0)[0]
    bmad = bmad[~np.isnan(bmad)]
    if bmad.size == 0:
        return None

    m = {
        "madMean":      round(float(bmad.mean()), 2),
        "madWorst":     round(float(bmad.max()), 2),
        "badBlockFrac": round(float((bmad > SAD_THRESHOLD_PX).mean()), 4),
        "blocks":       int(bmad.size),
    }
    m["pass"] = (m["madMean"] <= GHOST_TOLERANCE["madMean"]
                 and m["badBlockFrac"] <= GHOST_TOLERANCE["badBlockFrac"])
    return m


SAD_THRESHOLD_PX = 18   # a block differing by more than this is "bad" (ghosted)


def score_scroll(report, gt):
    gt_ev = gt_scroll_events(gt)
    algo_ev = {e["frame"]: e["dy"] for e in report.get("scrollEvents", [])}
    frames = set(gt_ev) | set(algo_ev)
    m = {
        "gtEvents":       len(gt_ev),
        "algoEvents":     len(algo_ev),
        "eventCountDiff": abs(len(algo_ev) - len(gt_ev)),
        "dyErrorSum":     sum(abs(algo_ev.get(f, 0) - gt_ev.get(f, 0)) for f in frames),
        "pageChanges":    report.get("transitions", {}).get("pageChange", -1),
    }
    m["pass"] = (m["dyErrorSum"] <= SCROLL_TOLERANCE["dyErrorSum"]
                 and m["eventCountDiff"] <= SCROLL_TOLERANCE["eventCountDiff"])
    return m


# ── Single-session scoring ────────────────────────────────────────────────────
def score_single(report, gt):
    pages = report.get("pages", [])
    seg_ok = len(pages) == 1 and report.get("transitions", {}).get("pageChange", -1) == 0

    p0 = pages[0] if pages else {"width": 0, "docHeight": 0, "fixedRects": []}
    bands = fixed_from_rects(p0["fixedRects"])

    # The stitch can only contain what the session SAW: expected height is the
    # deepest scroll + viewport (== pageHeight when the session reached the
    # bottom). A session that never scrolled expects a viewport-sized stitch.
    seen_max = max((f["scrollY"] for f in gt["frames"]), default=0)
    expected = {
        "width":  gt["viewport"]["width"],
        "height": seen_max + gt["viewport"]["height"],
        "header": gt["fixed"]["headerHeight"], "footer": gt["fixed"]["footerHeight"],
        "left":   gt["fixed"]["leftWidth"],    "right":  gt["fixed"]["rightWidth"],
    }
    measured = {"width": p0["width"], "height": p0["docHeight"], **bands}

    # Without scrolling, fixed elements are unknowable — the tool must not
    # CLAIM any (expected bands become 0, exact).
    no_scroll = seen_max < 150
    metrics = {}
    for k in expected:
        exp = 0 if (no_scroll and k in ("header", "footer", "left", "right")) else expected[k]
        tol = 0 if (no_scroll and k in ("header", "footer", "left", "right")) else TOLERANCE[k]
        err = measured[k] - exp
        metrics[k] = {"measured": measured[k], "expected": exp,
                      "error": err, "within_tol": abs(err) <= tol}
    total_error = sum(abs(metrics[k]["error"]) for k in metrics)
    stitch_pass = all(metrics[k]["within_tol"] for k in metrics) and seg_ok
    return metrics, total_error, stitch_pass, seg_ok


def print_single(metrics, total_error, stitch_pass, scroll):
    print(f"  {'metric':<8}{'measured':>10}{'expected':>10}{'error':>8}   status")
    print("  " + "-" * 46)
    for k in ("width", "height", "header", "footer", "left", "right"):
        m = metrics[k]
        flag = "ok " if m["within_tol"] else "OFF"
        print(f"  {k:<8}{m['measured']:>10}{m['expected']:>10}{m['error']:>+8}   {flag}")
    print("  " + "-" * 46)
    print(f"  totalError = {total_error}px    stitch: {'PASS' if stitch_pass else 'FAIL'}")
    print(f"\n  Scroll events:  gt={scroll['gtEvents']}  algo={scroll['algoEvents']}"
          f"  countDiff={scroll['eventCountDiff']} (tol {SCROLL_TOLERANCE['eventCountDiff']})")
    print(f"  dyErrorSum = {scroll['dyErrorSum']}px (tol {SCROLL_TOLERANCE['dyErrorSum']})"
          f"    pageChanges = {scroll['pageChanges']} (must be 0)"
          f"    scroll: {'PASS' if scroll['pass'] else 'FAIL'}")


# ── Multipage-session scoring ─────────────────────────────────────────────────
def score_multipage(report, gt):
    T = MULTIPAGE_TOLERANCE
    vw, vh = gt["viewport"]["width"], gt["viewport"]["height"]
    r_pages, g_pages = report.get("pages", []), gt["pages"]
    checks = []            # (name, measured, expected, err, within)
    total_error = 0

    def check(name, measured, expected, tol):
        nonlocal total_error
        err = measured - expected
        total_error += abs(err)
        checks.append({"name": name, "measured": measured, "expected": expected,
                       "error": err, "within_tol": abs(err) <= tol})

    check("pageCount", len(r_pages), len(g_pages), 0)

    # Navigation boundaries, types (NAVIGATE|REVISIT) and revisit entry offsets.
    r_navs, g_navs = report.get("navigations", []), gt["navigations"]
    check("navCount", len(r_navs), len(g_navs), 0)
    visited = {gt["visits"][0]["page"]} if gt.get("visits") else {0}
    for k, gnav in enumerate(g_navs):
        rnav = r_navs[k] if k < len(r_navs) else {}
        check(f"nav{k}.boundary", rnav.get("frame", -999), gnav["firstStableFrame"], T["boundaryFrames"])

        expected_type = "REVISIT" if gnav["toPage"] in visited else "NAVIGATE"
        visited.add(gnav["toPage"])
        check(f"nav{k}.type", 0 if rnav.get("type") == expected_type else 1, 0, 0)

        if expected_type == "REVISIT":
            gt_vt = gt["frames"][gnav["firstStableFrame"]]["scrollY"]
            check(f"nav{k}.vtStart", rnav.get("vtStart", -999), gt_vt, T["vtStartPx"])

    # Per-page geometry vs what the visit actually SAW (not the full page —
    # content never scrolled to is unknowable).
    fixed_na = []
    for i, gpage in enumerate(g_pages):
        seen = [f["scrollY"] for f in gt["frames"] if f.get("page") == i]
        seen_max = max(seen) if seen else 0
        if i >= len(r_pages):
            check(f"page{i}.width", 0, vw, T["widthPx"])
            check(f"page{i}.height", 0, seen_max + vh, T["heightPx"])
            continue
        rp = r_pages[i]
        check(f"page{i}.width", rp["width"], vw, T["widthPx"])
        check(f"page{i}.height", rp["docHeight"], seen_max + vh, T["heightPx"])
        if seen_max >= T["minScrollForFixed"]:
            bands = fixed_from_rects(rp["fixedRects"])
            gfix = gpage["fixed"]
            for band, key in (("header", "headerHeight"), ("footer", "footerHeight"),
                              ("left", "leftWidth"), ("right", "rightWidth")):
                check(f"page{i}.{band}", bands[band], gfix[key], T["fixedPx"])
        else:
            fixed_na.append(i)

    passed = all(c["within_tol"] for c in checks)
    return checks, total_error, passed, fixed_na


def print_multipage(checks, total_error, seg_pass, fixed_na, scroll):
    print(f"  {'check':<18}{'measured':>10}{'expected':>10}{'error':>8}   status")
    print("  " + "-" * 56)
    for c in checks:
        flag = "ok " if c["within_tol"] else "OFF"
        print(f"  {c['name']:<18}{c['measured']:>10}{c['expected']:>10}{c['error']:>+8}   {flag}")
    print("  " + "-" * 56)
    if fixed_na:
        print(f"  fixed bands not scored for page(s) {fixed_na} — visit scrolled "
              f"< {MULTIPAGE_TOLERANCE['minScrollForFixed']}px (unknowable without scrolling)")
    print(f"  totalError = {total_error}px    segmentation: {'PASS' if seg_pass else 'FAIL'}")
    print(f"\n  Scroll events:  gt={scroll['gtEvents']}  algo={scroll['algoEvents']}"
          f"  countDiff={scroll['eventCountDiff']}  dyErrorSum={scroll['dyErrorSum']}px"
          f"    scroll: {'PASS' if scroll['pass'] else 'FAIL'}")


# ── --all: score every recorded session, print a summary ─────────────────────
def run_all(strict):
    import glob as _glob
    sessions = sorted(d for d in _glob.glob(os.path.join(HERE, "session_*"))
                      if os.path.exists(os.path.join(d, "ground_truth.json")))
    if not sessions:
        die("no recorded sessions found (session_*/ground_truth.json)")
    results = []
    for s in sessions:
        # Children always run strict so the summary shows true pass/fail.
        cmd = [sys.executable, os.path.abspath(__file__), os.path.basename(s), "--strict"]
        rc = subprocess.run(cmd).returncode
        results.append((os.path.basename(s), rc))
    print("═" * 46)
    print("  Session summary")
    for name, rc in results:
        print(f"    {name:<22} {'PASS' if rc == 0 else 'FAIL'}")
    print("═" * 46)
    sys.exit(1 if strict and any(rc for _, rc in results) else 0)


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    strict = "--strict" in sys.argv
    if "--all" in sys.argv:
        run_all(strict)
        return
    session = os.path.join(HERE, args[0]) if args else os.path.join(HERE, "session_page1")
    video = os.path.join(session, "session.mp4")
    gt_path = os.path.join(session, "ground_truth.json")
    out_dir = os.path.join(session, "outputs")
    work_png = os.path.join(out_dir, "pending.png")
    work_json = os.path.join(out_dir, "pending.json")

    if not os.path.exists(BINARY):
        die(f"binary not found: {BINARY}\n  build it first:  cmake --build {os.path.join(REPO,'build')} -j4")
    for p in (video, gt_path):
        if not os.path.exists(p):
            die(f"missing: {p}  (record + mux the session first)")

    gt = json.load(open(gt_path))
    multipage = "pages" in gt

    # ── Run the tool ───────────────────────────────────────────────────────────
    os.makedirs(out_dir, exist_ok=True)
    proc = subprocess.run([BINARY, video, "-o", work_png, "-r", work_json, "-v"],
                          capture_output=True, text=True)
    log = proc.stdout + proc.stderr
    if proc.returncode != 0:
        die(f"verty exited {proc.returncode}\n{log}")
    if not os.path.exists(work_json):
        die("tool did not write the session report (-r)")
    report = json.load(open(work_json))

    # ── Score ──────────────────────────────────────────────────────────────────
    scroll = score_scroll(report, gt)
    ghost = None if multipage else score_ghost(gt, session, work_png)
    if multipage:
        checks, total_error, seg_pass, fixed_na = score_multipage(report, gt)
        passed = seg_pass and scroll["pass"]
    else:
        metrics, total_error, stitch_pass, seg_ok = score_single(report, gt)
        passed = (stitch_pass and scroll["pass"] and scroll["pageChanges"] == 0
                  and (ghost is None or ghost["pass"]))

    # ── Archive outputs under unique, self-describing names ───────────────────
    now = datetime.now(timezone.utc)
    gi = git_info()
    tag = (gi["commit"] or "nogit") + ("-dirty" if gi["dirty"] else "")
    base = f"run_{now.strftime('%Y%m%dT%H%M%SZ')}_{tag}_err{total_error}"
    outputs = []
    # the tool writes pending.png for one page, pending_pageN.png for several
    candidates = [("pending.png", f"{base}.png")] + \
                 [(f"pending_page{i}.png", f"{base}_page{i}.png") for i in range(len(report.get("pages", [])))]
    for src, dst in candidates:
        s = os.path.join(out_dir, src)
        if os.path.exists(s):
            d = os.path.join(out_dir, dst)
            os.replace(s, d)
            outputs.append(os.path.relpath(d, REPO))
    report_path = os.path.join(out_dir, base + ".json")
    os.replace(work_json, report_path)

    entry = {
        "timestamp": now.isoformat(timespec="seconds"),
        "git": gi,
        "session": os.path.relpath(session, REPO),
        "type": "multipage" if multipage else "single",
        "video": os.path.relpath(video, REPO),
        "outputs": outputs,
        "report": os.path.relpath(report_path, REPO),
        "totalError": total_error,
        "scroll": scroll,
        "pass": passed,
    }
    if multipage:
        entry["checks"] = checks
    else:
        entry["metrics"] = metrics
    if ghost is not None:
        entry["ghost"] = ghost

    # ── History (same session only) ────────────────────────────────────────────
    history = []
    if os.path.exists(RESULTS_LOG):
        with open(RESULTS_LOG) as f:
            history = [json.loads(l) for l in f if l.strip()]
    same = [e for e in history if e.get("session") == entry["session"]]
    prev = same[-1] if same else None
    best = min(same, key=lambda e: e["totalError"]) if same else None

    with open(RESULTS_LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")

    # ── Print ──────────────────────────────────────────────────────────────────
    print(f"\nStitch benchmark — {entry['session']} ({entry['type']})   "
          f"[git {gi['commit'] or '?'}{'*' if gi['dirty'] else ''}]")
    if multipage:
        print_multipage(checks, total_error, seg_pass, fixed_na, scroll)
    else:
        print_single(metrics, total_error, stitch_pass, scroll)
    if ghost is not None:
        print(f"\n  Ghost score vs fullpage.png:  madMean={ghost['madMean']} "
              f"(tol {GHOST_TOLERANCE['madMean']})  madWorst={ghost['madWorst']}"
              f"  badBlocks={ghost['badBlockFrac']*100:.2f}% "
              f"(tol {GHOST_TOLERANCE['badBlockFrac']*100:.0f}%)"
              f"    ghost: {'PASS' if ghost['pass'] else 'FAIL'}")
    print(f"\n  overall: {'PASS' if passed else 'FAIL'}")

    if prev is not None:
        d = total_error - prev["totalError"]
        arrow = "→ same" if d == 0 else (f"↓ improved by {-d}px" if d < 0 else f"↑ regressed by {d}px")
        print(f"  vs previous run ({prev['totalError']}px, {prev['timestamp'][:10]}): {arrow}")
    if best is not None and best["totalError"] < total_error:
        print(f"  best so far: {best['totalError']}px ({best['git'].get('commit','?')}, {best['timestamp'][:10]})")
    elif same:
        print(f"  ★ new best (or tied) — {total_error}px")
    print(f"\n  logged to {os.path.relpath(RESULTS_LOG, REPO)}  ({len(history)+1} runs total)")
    for o in outputs:
        print(f"  stitched PNG archived: {o}")
    print(f"  session report archived: {entry['report']}\n")

    sys.exit(1 if (strict and not passed) else 0)


if __name__ == "__main__":
    main()
