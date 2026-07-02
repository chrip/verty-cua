# Test data — scrolling-session fixtures

Realistic web-page fixtures and a recorder for generating **ground-truth test
datasets** for the full-page screenshot stitcher. Unlike the synthetic
`gen_test*.py` generators in the project root, these use a **real browser
render** (real fonts, anti-aliasing, hover CSS) and log the exact scroll offset
of every frame so the stitcher's output can be scored against known truth.

## Contents

| File | What it is |
|------|------------|
| `scroll_page.html` | Website 1 — fixed header/footer/left/right chrome + a tall scrolling article. Links → page 2. |
| `scroll_page_2.html` | Website 2 — **byte-identical chrome**, a *different* article, links → page 1. |
| `record_session.mjs` | Records a scroll session of a page → JPG frames + `ground_truth.json` (headless Chromium via CDP). |
| `frames_to_mp4.py` | Assembles recorded JPG frames into an `.mp4` (OpenCV `mp4v`, no ffmpeg needed). |
| `test_stitch_session.py` | Regression benchmark: runs the stitcher on a session and scores its output against the ground truth. |
| `stitch_test_results.jsonl` | Append-only history of benchmark runs — track `totalError` as the algorithm improves. |
| `session_page1/` | Example generated dataset (frames + mp4 + ground truth). |

Both pages have the same layout dimensions: **header 72px, footer 56px, left
sidebar 240px, right sidebar 180px**, at a 1280×900 viewport. Every link on a
page navigates to the *other* page, so you can record a session that scrolls,
clicks through, and scrolls again — a test of fixed-chrome deduplication across
a navigation.

## Requirements

- `chromium` on `PATH` (used headlessly)
- Node.js **22+** (uses built-in `WebSocket`/`fetch` — no `npm install`)
- Python 3 with `opencv-python` (already a project dependency)

## Quick start

```bash
cd test_data

# 1. Record page 1 scrolling top → bottom  (→ session_page1/frames/ + ground_truth.json)
node record_session.mjs

# 2. Assemble the frames into an mp4        (→ session_page1/session.mp4)
python3 frames_to_mp4.py

# 3. Feed it to the stitcher
../build/verty session_page1/session.mp4 -o out.png
```

### Recording the other page

`record_session.mjs [pageFile] [outDir]` — both args optional:

```bash
node record_session.mjs scroll_page_2.html session_page2
python3 frames_to_mp4.py session_page2          # → session_page2/session.mp4
```

## What a recording produces

```
session_page1/
├── frames/
│   ├── frame_00000.jpg      # zero-padded, in scroll order
│   ├── frame_00001.jpg
│   └── …
├── ground_truth.json        # per-frame scroll offset + fixed-region truth
└── session.mp4              # built by frames_to_mp4.py
```

The session scrolls in fixed pixel steps from `scrollY = 0` to the bottom, with
a short still pause at each end (so the stitcher's identical-frame pre-filter is
exercised).

### `ground_truth.json` schema

```jsonc
{
  "viewport":  { "width": 1280, "height": 900, "deviceScaleFactor": 1 },
  "scrollStep": 8,            // px scrolled per frame
  "fps": 30,                  // playback fps baked into the mp4
  "startPause": 12,           // still frames at top / bottom
  "endPause": 12,
  "frameCount": 319,
  "pageHeight": 3253,         // full document height — the stitch output target
  "maxScrollY": 2353,
  "fixed": {                  // convenience scalars for the algorithm
    "headerHeight": 72, "footerHeight": 56, "leftWidth": 240, "rightWidth": 180
  },
  "regions": {                // document-coordinate boxes of each fixed region
    "header": { "x": 0, "y": 0, "width": 1265, "height": 72 },
    "left":   { … }, "right": { … }, "footer": { … }, "scroll": { … }
  },
  "frames": [                 // exact scroll offset of every captured frame
    { "index": 0, "file": "frame_00000.jpg", "scrollY": 0 },
    …
  ]
}
```

Score the stitcher by comparing its output height against `pageHeight` and its
detected fixed-region sizes against `fixed`.

## Regression benchmark

`test_stitch_session.py` runs the built `verty` on a session's mp4 and
scores its output against that session's `ground_truth.json`, then appends the
result to `stitch_test_results.jsonl`. Run it whenever the algorithm changes to
see whether things got better.

```bash
cd test_data
python3 test_stitch_session.py                 # scores session_page1, logs a run
python3 test_stitch_session.py session_page2   # a different session
python3 test_stitch_session.py --strict        # exit non-zero if any metric off-tolerance
```

Scored metrics (pixels): `width`, `height` (reconstructed doc size vs. viewport
width / `pageHeight`) and `header`, `footer`, `left`, `right` (detected fixed
bands vs. ground truth). The headline number is **`totalError`** = sum of the
absolute per-metric errors — **lower is better**. Each run prints a table, the
delta vs. the previous run, and the best-so-far, e.g.:

```
  metric    measured  expected   error   status
  width         1280      1280      +0   ok
  height        3253      3253      +0   ok
  header          80        72      +8   ok
  footer          69        56     +13   ok
  left           280       240     +40   OFF
  right          224       180     +44   OFF
  totalError = 105px    result: FAIL
```

**Baseline (git `b22c9fd`): totalError 105px** — width/height exact; the four
fixed regions are over-detected (the pixel-perfect / content-aware refinement
work targets exactly this). As that lands, `totalError` should drop toward 0.
Tolerances for the pass/fail flag live in `TOLERANCE` at the top of the script
(size exact, fixed regions within 1 block = 16px).

Each run's stitched PNG is **archived** to
`<session>/outputs/run_<utc>_<commit>_err<N>.png` and linked from the log
entry's `output` field, so you can eyeball the actual reconstruction for any
past run — not just the numbers. (The archived PNGs are gitignored local
artifacts; the committed record is `stitch_test_results.jsonl`, with each run's
key metrics also encoded in its filename.)

## Tunables

Edit the constants at the top of `record_session.mjs`:

| Constant | Meaning |
|----------|---------|
| `VIEWPORT_W` / `VIEWPORT_H` | Exact frame size (device px, scale factor 1). |
| `SCROLL_STEP` | Pixels per frame. Smaller ⇒ smoother, more overlap, more frames. |
| `START_PAUSE` / `END_PAUSE` | Still frames held at top / bottom. |
| `FPS` | Playback fps recorded into `ground_truth.json` and the mp4. |
| `JPEG_QUALITY` | 0–100 (JPEG frame quality). |
| `SETTLE_MS` | Repaint wait after each scroll before capturing. |

## Ground truth in the browser (interactive)

Open either page directly in a browser and check the DevTools console — the
embedded JS echoes the same measurements live:

- `[GROUND TRUTH]` — page height, viewport, fixed-region boxes
- `[ALL ELEMENT BOUNDING BOXES]` — every element (document coordinates)
- `[scroll]` — each scroll event as it happens

Callable from the console: `logGroundTruth()`, `logAllBoundingBoxes()`,
`dumpScrollLog()`. Also on `window`: `GROUND_TRUTH`, `ALL_BOXES`, `SCROLL_LOG`.

## Notes

- **Deterministic vs. live capture.** This scripted recorder gives reproducible
  frames with an exact scroll↔frame mapping. If you instead want a human-paced
  live recording, install ffmpeg (`sudo apt install ffmpeg`) and run
  `ffmpeg -f x11grab -framerate 30 -i :1 -c:v libx264 out.mp4` while scrolling
  manually — but you lose the per-frame scroll ground truth.
- **No hover in recordings.** Scrolling is programmatic (no cursor), so link
  hover effects don't appear — expected for a scroll-only session.
- **Git hygiene.** `session.mp4` is covered by the root `.gitignore` (`*.mp4`);
  the `frames/*.jpg` are not. Add `test_data/session_*/frames/` to `.gitignore`
  if you don't want frame dumps tracked.
- **Chromium cleanup.** The recorder shuts its browser down via the CDP
  `Browser.close` command, so it does not leak processes.
