# verty

Reconstruct a **full-page screenshot** from a screen recording of someone
scrolling through a web page. The tool watches the video, works out how far the
page scrolled in each frame and which parts of the screen are fixed chrome
(header / footer / sidebars), then stitches the scrolling content back into one
tall image with the fixed elements stamped in place.

> ### Status: work in progress (proof of concept)
> **Milestone 1 is done** — a clean three-pass pipeline that reconstructs a page
> from a scroll session, plus a reproducible browser-based benchmark. This is a
> prototype, not a polished tool:
> - it targets **edge-anchored fixed chrome** (header / footer / left+right sidebars);
> - it assumes **one page per video**;
> - it's tuned and measured on lab recordings (see the benchmark below).
>
> **Next milestone:** detect page changes *within* a session (e.g. a navigation
> click loads a new page) and emit one stitched screenshot per page.

## How it works

Three passes over the video (`src/stitcher.{hpp,cpp}`):

1. **analyze** — accumulate the position-fixed pixel mask and a rough per-frame
   scroll offset; reduce the mask to header / footer / left / right fixed-band
   rectangles.
2. **refine_scrolls** — re-detect the scroll offsets, now excluding the fixed
   areas from the search so only genuine scrolling content votes.
3. **reconstruct** — copy each frame's scrolling interior into a tall canvas at
   its scroll offset, then stamp the fixed bands on top: header at the top,
   footer at the bottom, sidebars spanning the full height.

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

- `-o` — the stitched full-page PNG.
- `-r` — optional **session report** (JSON): scroll events with timestamps
  (`{t, frame, dy, vt}`), fixed-element rectangles, and a tally of transition
  types (identical / scroll / pageChange / unstable).

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
python3 test_stitch_session.py
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
src/            the tool — Stitcher pipeline (stitcher.*) + scroll detector
test_data/      HTML fixtures, session recorder, mp4 muxer, benchmark + results log
CMakeLists.txt
```
