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
| `record_session.mjs` | Records a session (scrolls, hovers, clicks/navigations) → JPG frames + `ground_truth.json` (headless Chromium via CDP). Plans: `single`, `multipage`. |
| `frames_to_mp4.py` | Assembles recorded JPG frames into an `.mp4` (OpenCV `mp4v`, no ffmpeg needed). |
| `test_stitch_session.py` | Regression benchmark: runs the stitcher on a session and scores stitch + scroll events against the ground truth. |
| `stitch_test_results.jsonl` | Append-only history of benchmark runs — track `totalError` as the algorithm improves. |
| `session_page1/` | Generated single-page dataset (frames + mp4 + ground truth) — the stitch benchmark session. |
| `session_multipage/` | Generated multi-page dataset: page 1 → scroll → hovers → click → page 2 (Milestone-3 fixture). |

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

`record_session.mjs [--plan single|multipage] [pageFile] [outDir]` — all optional:

```bash
node record_session.mjs scroll_page_2.html session_page2
python3 frames_to_mp4.py session_page2          # → session_page2/session.mp4
```

### Recording the multi-page session (Milestone-3 fixture)

```bash
node record_session.mjs --plan multipage        # → session_multipage/
python3 frames_to_mp4.py session_multipage      # → session_multipage/session.mp4
```

The `multipage` plan: arrive on page 1 → scroll a little (eased) → read →
hover a header link, a sidebar link, and the CTA (the real `:hover` CSS lands
in the video — non-scroll diffs the classifier must not misread) → **click the
CTA** (a real navigation to page 2, dispatched as CDP mouse events) → park the
mouse → stay on page 2 for ~2.5 s.

Its `ground_truth.json` uses the **multi-page schema**: `pages[]` (per page:
`pageHeight`, `maxScrollY`, `fixed`, `regions`), per-frame `{page, scrollY}`
(`page: null` marks navigation-transition frames), `events[]` (hover ranges,
click, pageLoaded), and `navigations[]` (`{fromPage, toPage, clickFrame,
firstStableFrame}`).

### Recording the revisit session (Milestone-4 fixture)

```bash
node record_session.mjs --plan revisit         # → session_revisit/
python3 frames_to_mp4.py session_revisit
```

Page 1 → scroll → click to page 2 → scroll → **history back** to page 1
(the browser restores the scroll position, so the revisit enters
mid-document) → scroll deeper than the first visit. Ground truth adds
`visits[]` (chronological, with `arrivedBy: initial|click|back`); `pages[]`
are unique pages. Scored additionally on `navK.type` (NAVIGATE|REVISIT) and
`navK.vtStart` (the recovered entry offset). Baseline (2026-07-03):
totalError 166 px — REVISIT recognized, vtStart error 0, merged page-0
height −4 px.

### Recording the hostile session (Milestone-5 fixture)

```bash
node record_session.mjs --plan hostile         # → session_hostile/ (scroll_page_hostile.html)
python3 frames_to_mp4.py session_hostile
```

Single-page scroll over a page with a continuously **animating region** and a
**floating fixed button** that appears only after 300px of scrolling. The
recorder additionally captures `fullpage.png` — the browser's full-page render
with the fixed chrome hidden (pure scrolling document; `captureBeyondViewport`
would otherwise paint position:fixed elements at viewport coordinates). The
benchmark scores a **ghost metric** against it: per-16px-block MAD, locally
aligned per block-row (±12px — drift is not ghosting), animated bboxes
excluded. Baseline (2026-07-04): madMean 3.84, badBlocks 1.13%.

### Axis-isolating sessions (2026-07-04)

```bash
node record_session.mjs --plan noscroll               # just look, never scroll
node record_session.mjs --plan single scroll_page_plain.html session_plain
node record_session.mjs --plan revisit_noscroll       # 2 pages + back, no scroll
node record_session.mjs --plan cookie                 # banner close = DOM mutation
```

`noscroll` expects a viewport-sized stitch with NO fixed elements claimed
(unknowable without scrolling). `plain` has no chrome at all. `revisit_noscroll`
checks navigation + REVISIT without scrolling or usable chrome. `cookie` is the
borderline case — run the tool with `-S normal` (one stitch) or `-S high`
(a new stitch per bigger mutation, no revisit merging) to see both valid
outcomes.

The benchmark auto-detects the schema and scores multipage sessions on:
page count, navigation count, per-navigation **boundary error** (first stable
frame vs. ground truth, tol ±3), per-page width/height vs. **what the visit
actually saw**, fixed bands (only for visits that scrolled ≥ 150px — a
no-scroll visit cannot reveal fixed elements), plus the session-wide
scroll-event score. Baseline (2026-07-03): **totalError 82px** — boundary
error 0, page heights −2/0, dyErrorSum 2px; sidebar bands +39 (the documented
over-detection).

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
python3 test_stitch_session.py session_hostile # a different session
python3 test_stitch_session.py --all           # every recorded session + summary
python3 test_stitch_session.py --strict        # exit non-zero if off-tolerance
```

Scored metrics (pixels): `width`, `height` (reconstructed doc size vs. viewport
width / `pageHeight`) and `header`, `footer`, `left`, `right` (detected fixed
bands vs. ground truth). The headline number is **`totalError`** = sum of the
absolute per-metric errors — **lower is better**. Each run prints a table, the
delta vs. the previous run, and the best-so-far, e.g.:

```
  metric    measured  expected   error   status
  width         1280      1280      +0   ok
  height        3251      3253      -2   ok
  header          78        72      +6   ok
  footer          69        56     +13   ok
  left           279       240     +39   OFF
  right          219       180     +39   OFF
  totalError = 99px    stitch: FAIL

  Scroll events:  gt=252  algo=246  countDiff=6 (tol 8)
  dyErrorSum = 6px (tol 12)    pageChanges = 0 (must be 0)    scroll: PASS
```

**Stitch reference (three-pass pipeline): totalError 99px** — width/height
near-exact; the four fixed bands over-detect on flat margins adjacent to the
chrome. That over-detection is a **documented non-decision**: the margin only
absorbs stable pixels and is sourced from the top/bottom frame, so it is
invisible in the output; a content-aware detector was evaluated and rejected
(see `ROADMAP.md`). Tolerances for the pass/fail flags live in `TOLERANCE` /
`SCROLL_TOLERANCE` at the top of the script.

**Scroll-event score (Milestone 2):** the tool's `-r` session report is checked
against the recorder's per-frame scroll log:
- `dyErrorSum` — Σ |dy_algo − dy_gt| over the union of scrolling frames,
- `eventCountDiff` — |#events difference|,
- `pageChanges` — PAGE_CHANGE transitions; **must be 0** on a single-page
  session (false-positive guard).

Baseline (2026-07-03): `dyErrorSum=6`, `eventCountDiff=6`, `pageChanges=0` —
all six misses are `|dy|=1` steps, rejected as noise by the ScrollDetector's
`|dy|≤1` gate **by design**; they also account for the stitch's −2px height.

Each run's stitched PNG **and session report** are archived to
`<session>/outputs/run_<utc>_<commit>_err<N>.{png,json}` and linked from the
log entry's `output`/`report` fields, so you can inspect the actual
reconstruction and event trace for any past run — not just the numbers. (The
archived files are gitignored local artifacts; the committed record is
`stitch_test_results.jsonl`, with each run's key metrics also encoded in its
filename.)

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
