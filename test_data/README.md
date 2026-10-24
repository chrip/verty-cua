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
| `session_fling/` | Generated single-page dataset with scroll LEAPS beyond the shared-pixel window — the intra-page re-localization fixture (M7). |
| `session_manyjumps/` | Same, ten leaps → ELEVEN visits on one page — the fixture that makes per-visit folding cost visible (M7). |

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

Since 2026-07-28 `cookie` also captures a **settled** `fullpage.png` (taken after
the session, banner dismissed, chrome hidden) and is scored by the ghost metric.
That makes one specific question measurable: does the stitch show the page as it
SETTLED, or does it freeze a transient early appearance? Under `-S normal` the
banner is a minority appearance (35 frames of 110) and must NOT survive into the
stitch. Baseline: madMean 2.57, madWorst 4.32, badBlocks 0.00%. The same
criterion scores 6.14 / 194.18 / 4.56% under the old first-observation rule, so
it genuinely discriminates.

### Intra-page re-localization (2026-07-27)

```bash
node record_session.mjs --plan fling                 # → session_fling/
python3 frames_to_mp4.py session_fling
```

Scroll the whole document, then LEAP across it in a single frame — back to the
top (−2353px), scroll down again, then leap to the bottom (+1647px). Each leap
is beyond `MAX_SCROLL` **and** beyond the physical limit of frame-to-frame
matching (`viewport − header − footer` ≈ 772px here), so the frames either side
share **no pixels**: to anything looking at two frames, a leap is
indistinguishable from a navigation. Both leaps land in territory the session
has already seen, so the page's own stitched canvas is the evidence — this is
the **content** path of re-localization.

What must hold is the OUTCOME, not the classification: exactly **one page**,
**zero navigation events**, and each leap present as one large **scroll event**.
`PAGE_CHANGE` transitions are expected here, so `session_fling` is listed in
`PAGECHANGE_EXPECTED` at the top of `test_stitch_session.py` — the guard becomes
"no navigation events" instead. Baseline (2026-07-27): totalError 79px,
`dyErrorSum` **1px**, `eventCountDiff` 0, 2 re-localized visits. Before
re-localization the same recording scored `dyErrorSum` 4000px with two bogus
REVISIT navigations, so the fixture genuinely fails without the feature.

The **scrollbar** path cannot be exercised in the lab: CDP screenshots contain
no scrollbar (verified — the right-edge modal value is page white in 100% of
frames of every fixture). `realworld/rw4_portal` is its only test.

```bash
node record_session.mjs --plan manyjumps            # → session_manyjumps/
python3 frames_to_mp4.py session_manyjumps
```

`manyjumps` is the same idea at SCALE: see the whole document once, then leap
between its two ends ten times, so eleven visits land on a SINGLE page. Its
correctness bar is the same (one page, zero navigations, the leaps as large
scroll events), but what it exists to measure is the COST of folding a visit into
the page canvas. `reconstruct()` runs after every visit, so rebuilding the whole
page each time is quadratic in visits-per-page; this fixture makes that visible
(59.5s before the incremental fold of 2026-07-29, 21.6s after — while every
stitch in the suite stayed byte-identical). Each run's wall clock is printed and
logged to `stitch_test_results.jsonl` as `elapsedSec`; it is never asserted,
because it is machine-dependent.

### In-place change: a gallery is not a video ad (2026-07-30)

```bash
node record_session.mjs --plan gallery              # → session_gallery/
python3 frames_to_mp4.py session_gallery
```

An embedded 320×260 panel — a prototype screen flow, the lab stand-in for a
phone-screen recording inside a study page — is tapped through **30 times**, each
tap followed by an 18-frame reading pause, from a scrolled position ~1000px down
the document. Then the session scrolls to the bottom and back to the top.

Per pixel this is indistinguishable from a fixed-position video ad: the same
pixels change again and again with no scroll. The separator is the **rate** —
30 changes in ~550 non-scroll transitions is 5.5%, where the corpus's real ad
sits at 23.6%. The fixture is dimensioned so that both the count (30 > the
25-frame confirm threshold) and the rate (5.5% < 10%) are unambiguous, and so
that the swap spreads over 0.37 of the non-chrome bands — under the 0.5
page-change gate, so the session must stay **one page** with **zero
navigations**.

It fails on the code that preceded it, in three independent ways:

| variant | capturedExtent (truth 1971) | panel claimed viewport-fixed | ghost madMean |
|---|---|---|---|
| before | 1457 | yes | 64.7 |
| rate gate missing only | 1971 | yes | 4.5 |
| both gates | **1971** | **no** | **4.5** |

- `page0.mutRegionClaimedFixed` is a new check: a page whose ground truth
  declares a `mutatingRegion` declares by the same token that the region is not
  viewport-fixed, so a claimed fixed overlay of that shape is the failure being
  guarded — in-place change read as an ad, its pixels excluded from every write
  and stamped once at a viewport row.
- The 1457px truncation was a second bug the fixture uncovered: on every
  screen-1→2 tap the scroll search proposed dy = −320 and the veto ACCEPTED it,
  because the newly dark panel body happened to match the same-shade page header
  320px above — a clean median residual computed from 3 changed bands out of 18.
  The veto now also requires a minimum share of informative bands to have changed
  (`SCROLL_MIN_CHANGED_BAND_FRAC`). Baseline after the fix: `dyErrorSum` **0px**,
  176/176 scroll events, height exact, ghost madMean 3.07 / badBlocks 0.98%.

### The other branch: the same panel, playing (2026-07-30)

```bash
node record_session.mjs --plan gallery_video          # → session_gallery_video/
python3 frames_to_mp4.py session_gallery_video
```

The same page loaded with `#video`, which turns the same 320×260 panel at the same
position into a canvas that repaints every frame. Nothing is tapped. It exists so
the subframe decision — *does this region ever hold still* — is testable in both
directions on one axis:

| | in-place changes | settled states | verdict |
|---|---|---|---|
| `--plan gallery` (30 taps) | 40 | **30** | screen flow, states kept |
| `--plan gallery_video` | 381 | **0** | dynamic noise, nothing kept |

Both are asserted from the ground truth, which declares the panel as a
`mutatingRegion` in one plan and an `animated` `dynamicRegion` in the other:
`page0.mutRegionIsScreenflow` + `page0.mutRegionStates` (against the recorded tap
count) and `page0.animatedRegionIsNoise` + `page0.animatedRegionStates`.

The animation starts on the button, not on load, so the plan can scroll the panel
into view while it is still. Scrolling past something that is animating is a
different problem with its own fixture (`session_hostile`, the M5 lesson): the
noise mask lives in viewport space and cannot follow a region that moves. Measured
on a version of this plan that played while scrolling: the host document fractured
into 3 pages instead of 1 and lost 7 of 46 scroll events. Keeping the plan still is
what makes this fixture about one thing.

### A subframe that scrolls its own content (2026-07-31)

```bash
node record_session.mjs --plan gallery_scroll         # → session_gallery_scroll/
python3 frames_to_mp4.py session_gallery_scroll
```

The same gallery page with `#innerscroll`: the panel holds a **2502px** document
(40 numbered, individually coloured rows — a misplaced row is visible and no
detector can alias one onto another) inside a **216px** window, and the plan
scrolls that window `0 → 1257 → 2286 → 457` while the page never moves after
bringing the panel into view. The recorder's `scrollEl` step drives it and records
`innerScrollTop` per frame.

Two things must hold, and both are asserted:

| check | measured | meaning |
|---|---|---|
| `page0.height` | 1457 | the document is only what the OUTER scroll covered — it must not grow because something inside it scrolled |
| `innerDocCount` | 1 | one inner document, not one per pause |
| `innerDocExtent` | 2495 of 2495 | the stitch from a 209px window over 335 dead-reckoned steps (2433 before the scroll search tried the previous dy's neighbourhood first) |

The three gallery plans together are the decision tree in one page: `gallery`
settles into states (screen flow), `gallery_video` never settles (noise),
`gallery_scroll` scrolls (screen flow with a stitched inner document).

The benchmark auto-detects the schema and scores multipage sessions on:
page count, navigation count, per-navigation **boundary error** (first stable
frame vs. ground truth, tol ±3), per-page width/height vs. **what the visit
actually saw**, fixed bands (only for visits that scrolled ≥ 150px — a
no-scroll visit cannot reveal fixed elements), plus the session-wide
scroll-event score. Baseline (2026-07-03): **totalError 82px** — boundary
error 0, page heights −2/0, dyErrorSum 2px; sidebar bands +39 (the documented
over-detection).

## Coverage honesty: capturedExtent vs documentHeight

Since schema 3 the report distinguishes two numbers that were previously one, and
the benchmark enforces the distinction (`coverageHonest` in `score_realworld`):

| field | meaning |
|---|---|
| `capturedExtent` | how much document the frames actually covered — deepest observed offset + one viewport |
| `documentHeight` | the document's real height, from the calibrated scrollbar map, with its own `uncertaintyPx`; `null` when unknowable |
| `reachedBottom` | `true` / `false` / `null` |
| `driftPx` | dead-reckoning drift MEASURED against the scrollbar; `null` without an independent reference |

The check asserts a page cannot be read as a complete capture unless it says so:
where the height is known, the captured extent must not exceed it beyond the
declared uncertainty and `reachedBottom` must agree with that comparison; where
the height is unknown, `reachedBottom` must be `null` rather than implying
either. Measured on the corpus: rw3's page 2 reports 4327px captured of a
4986±23px document with `reachedBottom: false`, and rw1 — an auto-refreshing page
whose document length changes between reloads, so no single map fits — reports
both as `null`.

## Real-world corpus (private recordings)

`realworld/` holds five REAL screen recordings, made manually on a private
machine, each paired with reference full-page stitches of the same pages
captured by a DOM-instrumented tool (`production/*.jpg`). Those files are
personal data and are **not in git**; `realworld_manifest.json` (committed) is
the corpus description — geometry, the measured baseline, and the per-case
expectations the benchmark enforces — so anyone can reproduce the suite by
recording their own equivalents. `--all` includes these sessions when present.

Scoring is structural (runs, ≥1 page, plausible geometry), a printed comparison
against the production reference, **plus the per-case `expect` block in
`realworld_manifest.json`**: an exact page count and a height window per page.
Those are drift guards anchored on the measured baseline, not derived truth —
they exist because the structural checks alone could not tell a page-for-page
reconstruction from a fractured one (rw4 passed structurally while truncating
its deep page to 48% and inventing a third page). Each `expect` carries a `note`
saying what is enforced and what is only soft: production's page COUNT is not
comparable where its sensitivity setting differs (rw5: 19 stitches vs our 5) or
where it emits one stitch per visit rather than per distinct page (rw4), and a
production stitch height is only a document height when the session actually
reached the bottom (rw1 and rw3-page2 did not — their references are captured
extents).

| case | content | video | production ref |
|------|---------|-------|----------------|
| rw1_newsportal  | news portal, deep scroll        | 27s  | 1 stitch, 4744px |
| rw2_textpage    | dense text page                 | 14s  | 1 stitch, 3984px |
| rw3_landing     | marketing site, 3 pages         | 67s  | 3 stitches |
| rw4_portal      | dark dashboard, very deep       | 113s | 4 stitches, 11624px max |
| rw5_mobileframe | mobile flow in phone mockup     | 93s  | 19 stitches |

First measured baseline (2026-07-04): all five PASS structurally.
rw2's main stitch is **1px** off the production height (3980 vs 3981);
rw5 tracks production per-page heights within 8px. Known gaps feeding M7:
over-segmentation on slow progressive page loads, and large scroll jumps
beyond MAX_SCROLL (rw4: 4890 vs 11624).

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
