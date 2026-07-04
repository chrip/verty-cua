# verty

Reconstruct a **full-page screenshot** from a screen recording of someone
scrolling through a web page. The tool watches the video, works out how far the
page scrolled in each frame and which parts of the screen are fixed chrome
(header / footer / sidebars), then stitches the scrolling content back into one
tall image with the fixed elements stamped in place.

> ### Status: work in progress (proof of concept)
> **Milestones 1–6 are done** (see `ROADMAP.md`): multi-page sessions with
> page-change detection, REVISIT recognition (a returning page merges into its
> existing stitch at the recovered scroll offset), scroll events with
> timestamps, anti-ghosting for animated regions and floating fixed overlays,
> and a machine-readable session report (`-r`, schema v1). All measured by a
> reproducible browser-based benchmark (`test_data/`, four recorded sessions,
> one command scores everything).
>
> Still a prototype: fixed-band edges carry a declared uncertainty (bands only
> over-detect, harmlessly), and everything is tuned on lab recordings —
> **next: real-world screencasts (M7)**.

## How it works

The session pipeline (`src/session_processor.*`) first classifies every frame
transition — `IDENTICAL | SCROLL(dy) | PAGE_CHANGE | UNSTABLE` — while keeping
an online estimate of the fixed chrome (pixels that stay stable across scroll
transitions; reset at every page change). A `SessionSegmenter` folds that
stream into **page visits** (navigation transitions are debounced). Every
visit is then assigned to a page by the `PageRegistry`: a visit whose first
frame matches a known page's canvas (block voting, `PageIdentifier`) is a
**REVISIT** — it merges into that page's existing stitch at the recovered
entry scroll offset (browsers restore scroll on back-navigation, so revisits
may enter mid-document); otherwise it starts a new page. Each page is
reconstructed by its `PageStitcher` in three passes per visit:

1. **analyze** — accumulate the position-fixed pixel mask and a rough per-frame
   scroll offset; reduce the mask to header / footer / left / right fixed-band
   rectangles.
2. **refine_scrolls** — re-detect the scroll offsets, now excluding the fixed
   areas from the search so only genuine scrolling content votes.
3. **reconstruct** — copy each frame's scrolling interior into a tall canvas at
   its scroll offset, then stamp the fixed bands on top: header at the top,
   footer at the bottom, sidebars spanning the full height.

A multi-page session yields one stitched PNG per page (`out_page0.png`, …);
a single-page session writes exactly the `-o` path.

## Requirements

- **Tool:** a C++20 compiler, CMake ≥ 3.20, and OpenCV (with dev headers).
- **Benchmark harness:** Node ≥ 22, Python 3 with `opencv-python`, and
  `chromium` on `PATH` (used headlessly).

## Build

```bash
git clone <repo> && cd verty
cmake -B build
cmake --build build -j
```

This produces `./build/verty`.

## Run it on a recording

```bash
./build/verty path/to/video.mp4 -o page.png -r report.json -v
```

- `-o` — the stitched full-page PNG (page N of a multi-page session gets a
  `_pageN` suffix).
- `-r` — optional **session report** (JSON): pages (stitch file, geometry,
  visit frame range, fixed-element rectangles), navigation events, scroll
  events with timestamps (`{t, frame, pageId, dy, vt}`), and a tally of
  transition types (identical / scroll / pageChange / unstable).

## Run the benchmark — from `git clone` to results

The benchmark records a real headless-browser scroll session of a bundled HTML
page, reconstructs it with the tool, and scores the result against ground truth
measured live in the browser. The recording is regenerated locally (it is not
stored in git), so the flow is fully reproducible from a clean clone:

```bash
# 0. build the tool first (see Build above), then:
cd test_data

# 1. record a scroll session of scroll_page.html  → frames + ground_truth.json
node record_session.mjs

# 2. assemble the captured frames into an mp4
python3 frames_to_mp4.py

# 3. reconstruct + score against the ground truth
python3 test_stitch_session.py        # or: --all (score every recorded session)
```

Step 3 prints a per-metric stitch table with a headline **`totalError`** in
pixels (lower is better) **and** a scroll-event score (per-frame `dy` errors,
event counts, page-change false positives) checked against the browser's
ground-truth scroll log. Every run is appended to
`test_data/stitch_test_results.jsonl` (the tracked history, so runs stay
comparable as the algorithm changes); the reconstructed image and the session
report are archived under `test_data/session_page1/outputs/`.

Current best: **`totalError` ≈ 99 px** — document width/height are near-exact;
the remainder is intentional fixed-band over-detection (a deliberately harmless
margin — details in [`test_data/README.md`](test_data/README.md)).

See **[`test_data/README.md`](test_data/README.md)** for the HTML fixtures, the
recorder's options, the `ground_truth.json` format, and exactly how the
benchmark is scored.

## Layout

```
src/            the tool — SessionProcessor → SessionSegmenter → PageRegistry
                (PageIdentifier) → PageStitcher; TransitionClassifier,
                ScrollDetector, FixedAccumulator, VideoSource, SessionReport
test_data/      HTML fixtures, session recorder, mp4 muxer, benchmark + results log
CMakeLists.txt
```
