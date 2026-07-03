#!/usr/bin/env python3
"""
test_stitch_session.py — regression benchmark for the stitching algorithm.

Runs `verty` on a recorded scroll session and scores its output against
that session's ground_truth.json (measured in the real browser). Every run is
appended to stitch_test_results.jsonl so the metrics can be tracked as the
algorithm improves — lower `totalError` is better.

Scored metrics:
  Stitch (pixels):
    width, height   — reconstructed document size vs. viewport width / pageHeight
    header, footer  — detected fixed top/bottom band height vs. ground truth
    left, right     — detected fixed sidebar width vs. ground truth
  Scroll events (Milestone 2, from the tool's -r report JSON):
    dyErrorSum      — Σ |dy_algo − dy_gt| over the union of scrolling frames
    eventCountDiff  — |#events_algo − #events_gt|
    pageChanges     — PAGE_CHANGE transitions (must be 0 on a single-page session)

Usage:
    python3 test_stitch_session.py [sessionDir] [--strict]
        sessionDir  session folder (default: session_page1, next to this file)
        --strict    exit non-zero if any metric is outside tolerance
                    (default: exit 0 whenever a measurement was produced)
"""
import os, re, sys, json, subprocess
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BINARY = os.path.join(REPO, "build", "verty")
RESULTS_LOG = os.path.join(HERE, "stitch_test_results.jsonl")

# Per-metric pass tolerance (pixels). The fixed regions are block-granular in the
# current algorithm, so 1 block (16px) is the near-term target; size is exact.
TOLERANCE = {"width": 0, "height": 32, "header": 16, "footer": 16, "left": 16, "right": 16}

# Scroll-event tolerances (Milestone 2). Baseline measured 2026-07-03:
# dyErrorSum=6 and eventCountDiff=6 — all six misses are |dy|=1 steps, which the
# ScrollDetector rejects as noise BY DESIGN (|dy|≤1 gate); they also account for
# the stitch's -2px height drift. pageChanges=0. Tolerances = baseline + margin.
SCROLL_TOLERANCE = {"dyErrorSum": 12, "eventCountDiff": 8, "pageChanges": 0}


def score_scroll(report, gt):
    """Compare the tool's scroll events against the recorder's ground truth."""
    fr = gt["frames"]
    gt_ev = {}
    for i in range(1, len(fr)):
        d = round(fr[i]["scrollY"] - fr[i - 1]["scrollY"])
        if d:
            gt_ev[i] = d
    algo_ev = {e["frame"]: e["dy"] for e in report.get("scrollEvents", [])}

    frames = set(gt_ev) | set(algo_ev)
    dy_error_sum = sum(abs(algo_ev.get(f, 0) - gt_ev.get(f, 0)) for f in frames)
    page_changes = report.get("transitions", {}).get("pageChange", -1)

    m = {
        "gtEvents":       len(gt_ev),
        "algoEvents":     len(algo_ev),
        "eventCountDiff": abs(len(algo_ev) - len(gt_ev)),
        "dyErrorSum":     dy_error_sum,
        "pageChanges":    page_changes,
    }
    m["pass"] = (m["dyErrorSum"] <= SCROLL_TOLERANCE["dyErrorSum"]
                 and m["eventCountDiff"] <= SCROLL_TOLERANCE["eventCountDiff"]
                 and m["pageChanges"] == SCROLL_TOLERANCE["pageChanges"])
    return m


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


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    strict = "--strict" in sys.argv
    session = os.path.join(HERE, args[0]) if args else os.path.join(HERE, "session_page1")
    video = os.path.join(session, "session.mp4")
    gt_path = os.path.join(session, "ground_truth.json")
    out_dir = os.path.join(session, "outputs")   # archived stitched PNGs, one per run
    work_png = os.path.join(out_dir, "pending.png")
    work_json = os.path.join(out_dir, "pending.json")

    if not os.path.exists(BINARY):
        die(f"binary not found: {BINARY}\n  build it first:  cmake --build {os.path.join(REPO,'build')} -j4")
    for p in (video, gt_path):
        if not os.path.exists(p):
            die(f"missing: {p}  (record + mux the session first)")

    gt = json.load(open(gt_path))
    expected = {
        "width":  gt["viewport"]["width"],
        "height": gt["pageHeight"],
        "header": gt["fixed"]["headerHeight"],
        "footer": gt["fixed"]["footerHeight"],
        "left":   gt["fixed"]["leftWidth"],
        "right":  gt["fixed"]["rightWidth"],
    }

    # ── Run the algorithm ──────────────────────────────────────────────────────
    os.makedirs(out_dir, exist_ok=True)
    proc = subprocess.run([BINARY, video, "-o", work_png, "-r", work_json, "-v"],
                          capture_output=True, text=True)
    log = proc.stdout + proc.stderr
    if proc.returncode != 0:
        die(f"verty exited {proc.returncode}\n{log}")
    if not os.path.exists(work_json):
        die("tool did not write the session report (-r) — Milestone 2 contract")
    report = json.load(open(work_json))

    # ── Parse measured values ──────────────────────────────────────────────────
    m_size = re.search(r"doc size:\s*(\d+)\D+(\d+)\s*px", log)
    m_reg = re.search(r"fixed header:\s*(\d+)px\s+footer:\s*(\d+)px\s+"
                      r"left:\s*(\d+)px\s+right:\s*(\d+)px", log)
    if not m_size or not m_reg:
        die("could not parse algorithm output (run with -v):\n" + log)

    measured = {
        "width":  int(m_size.group(1)),
        "height": int(m_size.group(2)),
        "header": int(m_reg.group(1)),
        "footer": int(m_reg.group(2)),
        "left":   int(m_reg.group(3)),
        "right":  int(m_reg.group(4)),
    }

    # ── Score ──────────────────────────────────────────────────────────────────
    metrics = {}
    for k in expected:
        err = measured[k] - expected[k]
        metrics[k] = {"measured": measured[k], "expected": expected[k],
                      "error": err, "within_tol": abs(err) <= TOLERANCE[k]}
    total_error = sum(abs(metrics[k]["error"]) for k in metrics)
    stitch_passed = all(metrics[k]["within_tol"] for k in metrics)

    scroll = score_scroll(report, gt)
    passed = stitch_passed and scroll["pass"]

    # ── Archive stitched output + report under unique, self-describing names ────
    now = datetime.now(timezone.utc)
    gi = git_info()
    tag = (gi["commit"] or "nogit") + ("-dirty" if gi["dirty"] else "")
    base = f"run_{now.strftime('%Y%m%dT%H%M%SZ')}_{tag}_err{total_error}"
    archive_path = os.path.join(out_dir, base + ".png")
    report_path = os.path.join(out_dir, base + ".json")
    os.replace(work_png, archive_path)
    os.replace(work_json, report_path)

    entry = {
        "timestamp": now.isoformat(timespec="seconds"),
        "git": gi,
        "session": os.path.relpath(session, REPO),
        "video": os.path.relpath(video, REPO),
        "output": os.path.relpath(archive_path, REPO),   # this run's stitched PNG
        "report": os.path.relpath(report_path, REPO),    # this run's session report
        "metrics": metrics,
        "totalError": total_error,
        "scroll": scroll,
        "pass": passed,
    }

    # ── Compare against history (before appending this run) ─────────────────────
    history = []
    if os.path.exists(RESULTS_LOG):
        with open(RESULTS_LOG) as f:
            history = [json.loads(l) for l in f if l.strip()]
    prev = history[-1] if history else None
    best = min(history, key=lambda e: e["totalError"]) if history else None

    with open(RESULTS_LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")

    # ── Report ─────────────────────────────────────────────────────────────────
    print(f"\nStitch benchmark — {entry['session']}   "
          f"[git {entry['git']['commit'] or '?'}{'*' if entry['git']['dirty'] else ''}]")
    print(f"  {'metric':<8}{'measured':>10}{'expected':>10}{'error':>8}   status")
    print("  " + "-" * 46)
    for k in ("width", "height", "header", "footer", "left", "right"):
        m = metrics[k]
        flag = "ok " if m["within_tol"] else "OFF"
        print(f"  {k:<8}{m['measured']:>10}{m['expected']:>10}{m['error']:>+8}   {flag}")
    print("  " + "-" * 46)
    print(f"  totalError = {total_error}px    stitch: {'PASS' if stitch_passed else 'FAIL'}")

    print(f"\n  Scroll events:  gt={scroll['gtEvents']}  algo={scroll['algoEvents']}"
          f"  countDiff={scroll['eventCountDiff']} (tol {SCROLL_TOLERANCE['eventCountDiff']})")
    print(f"  dyErrorSum = {scroll['dyErrorSum']}px (tol {SCROLL_TOLERANCE['dyErrorSum']})"
          f"    pageChanges = {scroll['pageChanges']} (must be {SCROLL_TOLERANCE['pageChanges']})"
          f"    scroll: {'PASS' if scroll['pass'] else 'FAIL'}")
    print(f"\n  overall: {'PASS' if passed else 'FAIL'}")

    if prev is not None:
        d = total_error - prev["totalError"]
        arrow = "→ same" if d == 0 else (f"↓ improved by {-d}px" if d < 0 else f"↑ regressed by {d}px")
        print(f"  vs previous run ({prev['totalError']}px, {prev['timestamp'][:10]}): {arrow}")
    if best is not None and best["totalError"] < total_error:
        print(f"  best so far: {best['totalError']}px ({best['git'].get('commit','?')}, {best['timestamp'][:10]})")
    elif history:
        print(f"  ★ new best (or tied) — {total_error}px")
    print(f"\n  logged to {os.path.relpath(RESULTS_LOG, REPO)}  ({len(history)+1} runs total)")
    print(f"  stitched PNG archived: {entry['output']}")
    print(f"  session report archived: {entry['report']}\n")

    sys.exit(1 if (strict and not passed) else 0)


if __name__ == "__main__":
    main()
