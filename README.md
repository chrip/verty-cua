# verty

Reconstruct a **full-page screenshot** from a screen recording of someone
scrolling through a web page. The tool watches the video, works out how far the
page scrolled in each frame and which parts of the screen are fixed chrome
(header / footer / sidebars), then stitches the scrolling content back into one
tall image with the fixed elements stamped in place.

> ### Status: work in progress (proof of concept)
> **Milestones 1–6 are done and M7 (real-world hardening) is well along** (see
> `ROADMAP.md`): multi-page sessions with page-change detection, REVISIT
> recognition (a returning page merges into its existing stitch at the recovered
> scroll offset), **intra-page re-localization of scroll jumps too large for
> frame-to-frame matching** (flings, Page-Down chains, scrollbar-thumb drags),
> scroll events with timestamps, anti-ghosting for animated regions and floating
> fixed overlays, persistent-noise handling for fixed-position video ads, and a
> machine-readable session report (`-r`, schema v2). All measured by a
> reproducible benchmark: ten recorded lab sessions plus five real production
> screencasts, scored by one command.
>
> Still a prototype: fixed-band edges carry a declared uncertainty (bands only
> over-detect, harmlessly), and document regions a jump skipped were never on
> screen — they stay blank and are declared in the report rather than invented.

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
may enter mid-document); otherwise it starts a new page.

A third possibility exists because scroll tracking is a chain of frame-to-frame
deltas, and the chain breaks when one step is larger than the shared-pixel
window (`viewport − header − footer`). Past that, two consecutive frames have
nothing in common, and a fling — or a drag of the scrollbar thumb — looks
exactly like a navigation. Such a visit is **re-localized** back into the page
it continues: by content where the jump landed in territory the canvas already
covers, and otherwise from the **vertical scrollbar**, whose thumb position
gives the absolute offset and whose thumb height fingerprints the document.
The map from thumb position to document offset is calibrated from the page's own
content-tracked scrolls, so no platform scrollbar metrics are assumed. A
re-localized jump is reported as one large *scroll event* inside one page — not
as a navigation — and any document rows the jump skipped are left blank and
listed in the report's `gaps`, because pixels that were never captured cannot be
reconstructed.

Each page is reconstructed by its `PageStitcher` in three passes per visit:

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
- `-S` — sensitivity `normal|high` (default normal): `normal` starts a new
  page only on a real navigation; `high` starts a new stitched screenshot on
  every bigger DOM mutation (e.g. a closing cookie banner) and never merges
  revisits.
- `-r` — optional **session report** (JSON, `"schema": 2`): pages (stitch file,
  geometry, visit frame ranges with their entry offset + declared uncertainty +
  a `relocalized` flag, fixed-element rectangles, interior overlays, and `gaps`
  — document rows no frame ever showed), navigation events, scroll events with
  timestamps (`{t, frame, pageId, dy, vt}`), and a tally of transition types
  (identical / scroll / pageChange / unstable).

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
                (PageIdentifier, ScrollbarTracker + ScrollbarMap) →
                PageStitcher; TransitionClassifier, ScrollDetector,
                FixedAccumulator, VideoSource, SessionReport
test_data/      HTML fixtures, session recorder, mp4 muxer, benchmark + results log
CMakeLists.txt
```
