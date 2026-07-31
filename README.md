# verty

**Document coordinates from a screen recording alone.**

`verty` watches a plain screen recording of somebody using a web page — no DOM,
no browser extension, no accessibility tree, no SDK in the page — and recovers
*where they were*: how far down the document every frame sat, which page it was
on, which pages the session returned to and at what offset, the geometry of the
chrome that never moved, and one settled full-page reconstruction per distinct
page. Where a jump skipped content, or a sticky element hid rows at every
reachable offset, it says so instead of filling them in.

Everything is estimated from pixels, so it works on recordings of software you
do not control — and on recordings made long before anyone thought to instrument
anything.

> ### Status: work in progress (proof of concept)
> **Milestones 1–6 are done and M7 (real-world hardening) is well along** (see
> `ROADMAP.md`): multi-page sessions with page-change detection, REVISIT
> recognition (a returning page merges into its existing stitch at the recovered
> scroll offset), **intra-page re-localization of scroll jumps too large for
> frame-to-frame matching** (flings, Page-Down chains, scrollbar-thumb drags),
> scroll events with timestamps, anti-ghosting for animated regions and floating
> fixed overlays, **subframes** — a region that changes while the document does not
> is reported with its position, and if it settles into states (a gallery, an
> embedded prototype tapped through screen by screen) each state is written as its
> own image, while a region that never holds still is called noise and kept out of
> the document model — and a machine-readable session report (`-r`, schema 8). All
> measured by a reproducible benchmark: fourteen recorded lab sessions plus seven
> real screen recordings, scored by one command.
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
reconstructed. (`gaps` also covers rows a viewport-fixed element hid at every
reachable scroll offset — a full-width sticky nav bar permanently conceals the
document rows beneath it.)

Each page is reconstructed by its `PageStitcher` in three passes per visit:

1. **analyze** — accumulate the position-fixed pixel mask and a rough per-frame
   scroll offset; reduce the mask to header / footer / left / right fixed-band
   rectangles.
2. **refine_scrolls** — re-detect the scroll offsets, now excluding the fixed
   areas from the search so only genuine scrolling content votes.
3. **reconstruct** — copy each frame's scrolling interior into a tall canvas at
   its scroll offset, then stamp the fixed bands on top: header at the top,
   footer at the bottom, sidebars spanning the full height. The browser's
   scrollbar is chrome, not content: its column is excluded from the interior
   and painted from a thumb-free strip, because the thumb's position means
   nothing in document coordinates.

Most document rows are seen by many frames, and they do not always agree — a
page is still settling when a visit begins (banners up, late content still
shifting the layout). So every observation of a row **votes**, and the appearance
most frames agree on is the one kept; ties go to the later frame, because pages
settle over time. That is what stops a dismissed cookie banner or a
mid-reflow layout from being frozen into the result.

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

- `-o` — the stitched full-page PNG. A multi-page session writes one file per
  page, and everything belonging to a page carries its number, zero-padded and
  page-first, so that a file browser sorting by name shows a page followed by its
  own subframes:

  ```
  out_page000.png                        the page
  out_page000_sub000_doc000.png          a subframe of it, stitched (it scrolled)
  out_page000_sub000_state000.png        a settled state of that subframe
  out_page000_sub000_state001.png
  out_page001.png                        the next page
  out_page001_sub000_state000.png
  ```

  A page can hold several subframes (`sub000`, `sub001`, …) — an ajax gallery and
  a scrollable comment section below it are two — and a region that survives a
  navigation appears under each page it was on, with that page's share of its
  states.
- `-S` — sensitivity `normal|high` (default normal): `normal` starts a new
  page only on a real navigation; `high` starts a new stitched screenshot on
  every bigger DOM mutation (e.g. a closing cookie banner) and never merges
  revisits.
- `-r` — optional **session report** (JSON, `"schema": 8`). Per page: the stitch
  file, `capturedExtent` (how much document the frames actually covered),
  `documentHeight` (the document's real height where the scrollbar could be read,
  with its own uncertainty — `null` when unknowable), `reachedBottom`
  (`true`/`false`/`null`), `driftPx` (dead-reckoning drift measured against the
  scrollbar), visit frame ranges with their entry offset + declared uncertainty +
  a `relocalized` flag, fixed-element rectangles, interior overlays, and `gaps`
  (document rows for which no scrolling content was ever observed). Per session:
  navigation events, scroll events with timestamps, a transition tally, and
  `frames[]` — one row per frame with `{pageId, vt}` or `null` plus the reason
  there is no answer.

  **Two coordinate systems, and which fields are in which.** The stitch is a
  DOCUMENT canvas: its rows are document rows, `capturedExtent` tall. Everything
  measured off a video frame — `fixedRects`, `overlays`, a subframe's `rect` — is in
  VIEWPORT coordinates, where row 0 is the top of the frame, not of the document. The
  two coincide only at scroll offset 0. `x`, `w` and `h` never differ between them
  (nothing here models horizontal scrolling); only `y` does. So a viewport `y` plus
  the offset `vt` of the frame it was measured on gives the document row — and where
  the report hands you an image to place, it now states that row outright rather than
  making you join `frames[]` to find it. Note also that the stitch *re-anchors* what
  it stamps: a bottom-pinned overlay and the footer band are painted at
  `capturedExtent - (viewportHeight - y)`, and a fixed sidebar spans the whole
  document, so their viewport rectangles are not where they ended up either.

  Each page also carries `subframes[]`: every region that changed while the
  document did not, with its rectangle in viewport coordinates, `documentY` (the
  document row that rectangle sits at on this page — `null` when the region's
  evidence frames disagree on one offset, when none of them was localized, or when
  the region was measured to be fixed to the viewport), `anchoredTo` with its
  `anchorEvidence`, `offsetSpreadPx`, `inPlaceChanges` (transitions on which it
  changed while the document stood still, counted once per frame even when several
  parts of it repainted), and its `kind`. A `screenflow` lists its settled `states` — frame,
  timestamp, how long it was held, its own `documentY`, and the image file each was written to
  (`<out>_page000_sub000_state000.png`, …); a `noise` region lists none, because a
  playing video has no state worth keeping. Where a screen flow **scrolled its own
  content**, it also lists `innerDocuments`: the pipeline runs again on that
  rectangle alone, so each becomes a stitch taller than the subframe
  (`<out>_page000_sub000_doc000.png`, …) with its own `capturedExtent` in the
  subframe's document coordinates. What separates them is whether the region ever
  held still: measured, a fixed video ad 0% of its changes, an embedded phone
  screen 31-39%, a panel tapped 30 times 100%.

  A page image cannot show a screen flow in all of its states, so it shows one —
  the state the region was left in — and `stampedState` says which. Without that
  the page canvas has nothing true to show there at all: the per-row vote elects
  whichever appearance each row's frames agreed on, and with mutually exclusive
  screens and no majority, adjacent rows elect different ones and the region
  renders as a superposition. A consumer laying the states out itself knows from
  `stampedState` which one is already on the page.

  **`anchoredTo` is measured, not assumed.** Across each scroll transition the page
  made, the region's pixels are compared against the previous frame twice: at the
  rectangle shifted by that transition's `dy` (it moved with the document) and at the
  same rectangle (it stayed put in the viewport). Both hypotheses are scored the same
  way over the rows both can see, so a region that also repainted itself pays the
  same penalty under each — which is why it works on a playing video ad. Measured
  across the corpus, the chosen hypothesis is never worse than 0.754 MAD and the
  rejected one never better than 22.24, including the three regions that never hold
  still. `null` means no verdict: usually a page that never scrolled, so no evidence
  exists (`anchorEvidence.scorableTransitions: 0`). A viewport-fixed region has no
  single document row, which is why `documentY` is then `null` — the evidence for
  that is in the same object rather than left to be inferred.

  **To place a state over the page stitch**, take `x`, `w` and `h` from the
  subframe's `rect` and `y` from that state's `documentY` — a state's image is a crop
  of the subframe's rectangle, nothing else. It is emphatically *not* `changedRect`,
  which is what changed to produce the state (one tap repaints a whole panel, the
  next only its body): measured across the corpus, all 58 states of the sessions that
  have any report a `changedRect` that differs from the rectangle their own PNG
  covers, by up to 365px and a factor of 11 in area. That field was called `rect` up
  to schema 6, sitting next to `image`, which made it a trap; it is renamed for
  exactly that reason.

  The distinction between `capturedExtent` and `documentHeight` is the point.
  Only the first is derivable from the frames; a session that stopped early
  cannot tell from its own pixels how much it never reached. Reporting one under
  the name of the other would let a consumer read a third of a document as the
  whole of it.

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

Current best: **`totalError` ≈ 106 px** — document width/height are near-exact;
the remainder is intentional fixed-band over-detection (a deliberately harmless
margin — details in [`test_data/README.md`](test_data/README.md)).

See **[`test_data/README.md`](test_data/README.md)** for the HTML fixtures, the
recorder's options, the `ground_truth.json` format, and exactly how the
benchmark is scored.

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Chosen
deliberately: it is permissive enough for anyone to use, modify and ship this
(including commercially), it carries an explicit patent grant, and it is the
same licence OpenCV itself uses from 4.5 onward, so there is no friction between
this project and its one dependency.

This is a personal learning project, published as a worked example of building a
non-trivial computer-vision pipeline under a strict measurement discipline. If it
is useful to you, take it.

## The real-world corpus is not in this repository

The seven real screen recordings the benchmark scores against were recorded
manually on a private machine and are personal data, so neither they nor their
reference images are committed (see `.gitignore`).
`test_data/realworld_manifest.json` is the committed description of that corpus —
its geometry, the measured baseline, and the per-case expectations the benchmark
enforces — so the lab fixtures reproduce from a clean clone and the real-world
part reproduces for anyone who records their own equivalents.

## Layout

```
src/            the tool — SessionProcessor → SessionSegmenter → PageRegistry
                (PageIdentifier, ScrollbarTracker + ScrollbarMap) →
                PageStitcher; TransitionClassifier, ScrollDetector,
                FixedAccumulator, SubframeTracker + SubframeStitcher (the
                pipeline applied to a subframe's own rectangle),
                VideoSource, SessionReport
test_data/      HTML fixtures, session recorder, mp4 muxer, benchmark + results log
CMakeLists.txt
```
