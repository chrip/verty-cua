# Roadmap

## Context & goal

A production algorithm already exists **browser-side**, with full DOM and event
access. This project builds its **pixel-based counterpart**: it does **not
re-implement the same algorithm** — it **estimates the same output contract
from pixels alone** (an MP4 screencast), with no DOM, no browser events, and
therefore with honestly stated uncertainty where pixels cannot decide what the
DOM could.

**The output contract, per session video:**

1. **Scroll events** — y-direction deltas with **timestamps** (frame index × fps).
2. **Position-fixed elements** — bounding boxes of header / footer / sidebars
   per page (approximate by nature: flat margins adjacent to chrome are
   pixel-ambiguous — proven in Milestone 1).
3. **Smart stitched screenshots** — one tall image per *distinct* page, free of
   ghosting (no duplicated footers, no smeared dynamic content, no glitches).
4. **Page-change detection** — a session may visit multiple pages.
5. **Recurring-page recognition (REVISIT)** — a page seen before is recognized
   and its new frames **merge into the existing stitch** instead of producing a
   duplicate. (Navigation is classified only as `NAVIGATE` = new page or
   `REVISIT` = known page. Browser-history semantics like back/forward are not
   knowable from pixels and are explicitly out of scope.)

**Done so far (Milestone 1, commits `6c48e14` + `c0cdcdc`):** a clean 3-pass
single-page pipeline (`Stitcher`: analyze → refine_scrolls → reconstruct), a
mask-aware `ScrollDetector`, browser-based test fixtures + session recorder +
scored regression benchmark (`test_data/`, current best `totalError` = 99 px).
See `README.md` for build/run/benchmark instructions.

---

## ⚑ Standing goal: real-world robustness

The tool must work on **real screencasts**, not only on our lab recorder's
output. This is a **cross-cutting goal, not a final milestone**: real-world
recordings (mouse cursor, smooth/inertial scrolling, compression artifacts,
variable fps, animated pages) enter the test corpus **from Milestone 3
onwards**, and every milestone's exit criteria include "no regression on the
real-world corpus". Milestone 7 is the dedicated hardening pass, but robustness
is earned continuously — a milestone that only works on synthetic fixtures is
not done.

**Corpus arrived (2026-07-04):** five real screen recordings, made manually on a
private machine and paired with reference stitches of the same pages from a
DOM-instrumented tool, live in `test_data/realworld/` —
see `realworld_manifest.json` and test_data/README.md. First baseline: all
five pass structurally; rw2's stitch is 1px off the production height; rw5
(mobile-in-frame) tracks production per-page heights within 8px. Measured
gaps that define the M7 backlog: (a) **over-segmentation on slow progressive
page loads** (real navigations render over many frames; our instant file://
fixtures never exercised this), (b) **large scroll jumps** beyond
MAX_SCROLL=400 are rejected and truncate deep pages (rw4: 4890 vs 11624),
(c) production sensitivity semantics differ per web element
(screenshotSensitivity column) — page-count comparisons are soft.

---

## Development model: built by AI coding agents

This project is developed with coding agents (Claude et al.). The repository is
structured so that agents **cannot drift** and the code stays a foundation for
production software, maintainable and optimizable by human *and* AI:

- **Machine-checkable goals.** Every milestone defines exit criteria as
  *measurable numbers from the benchmark*, never as prose judgment. An agent
  (or human) verifies "done" by running one command, not by opinion.
- **Tests before algorithms.** Each milestone FIRST extends the recorder +
  benchmark so the new capability is measurable, THEN implements it. If the
  exit criterion cannot be measured yet, the milestone hasn't started.
- **Drift detection.** `test_data/stitch_test_results.jsonl` is the committed,
  append-only history of benchmark runs (timestamp + git commit + scores).
  Any change that regresses it must justify itself explicitly.
- **Determinism.** Seeded RNG, no wall-clock dependence in algorithms, fixed
  fixtures — the same input must give the same output on every run, or results
  across agent sessions are not comparable.
- **Context anchors.** `README.md` (how to build/run/benchmark), `ROADMAP.md`
  (what to build next and why), `test_data/README.md` (how testing works) are
  kept current — they are the onboarding context for every future agent
  session. A milestone is not done until they reflect it.
- **Code rules** (established in Milestone 1, standing):
  - one class = one concern; small files; explicit passes; no hidden global state;
  - cleanliness over speed — **no speculative optimization**; optimize only
    with a measured reason;
  - warning-clean builds (`-Wall -Wextra`);
  - deliberate non-decisions are documented in code comments and in this file,
    so they don't get re-litigated by the next agent (example: fixed-band
    over-detection is intentionally left — the margin only absorbs stable
    pixels and is provably invisible in the output; a content-aware detector
    was evaluated and rejected as compute + risk for a number-only gain).

**Definition of done, every milestone:** builds warning-clean · all benchmark
scores meet the milestone's stated numbers · no regression in
`stitch_test_results.jsonl` (synthetic *and* real-world corpus) · docs updated
· no unrelated changes in the diff.

---

## Target architecture (evolves per milestone)

```
main.cpp
 └── SessionProcessor            orchestrates the whole session          (M3)
      ├── VideoSource            frames + timestamps from the mp4        (M2)
      ├── TransitionClassifier   IDENTICAL | SCROLL(dy) | PAGE_CHANGE |
      │                          UNSTABLE (loading/animation)            (M2)
      ├── SessionSegmenter       transitions → page-visit segments       (M3)
      ├── PageRegistry           page fingerprints, identity, intra-page
      │                          re-localization of large jumps       (M4/M7)
      │    ├── PageIdentifier    frame ↔ known-page matching             (M4)
      │    └── ScrollbarTracker  reads the vertical scrollbar; with
      │         + ScrollbarMap   a per-page thumb→offset calibration,
      │                          the absolute offset of a frame whose
      │                          delta chain broke                      (M7)
      ├── PageStitcher           today's Stitcher, scoped to a segment,
      │                          mergeable across visits                 (M3/M4)
      │    └── ScrollDetector    (exists; mask-aware)
      └── SessionReport          JSON output of the full contract        (M6)
```

Principles: each class owns one concern and is testable in isolation; passes
over the video stay explicit and deterministic; no hidden global state.

---

## Milestone 2 — Transition classification + scroll-event export ✅ (2026-07-03)

**Goal:** every frame-to-frame transition gets a typed classification, and
scroll events with timestamps become a first-class output.

**Status: done.** `VideoSource`, `TransitionClassifier`, `SessionReport` (first
cut, `-r report.json`) implemented; `Stitcher` refactored onto them. Measured
exit criteria on the single-page benchmark: stitch unchanged (99 px),
`pageChanges = 0`, `dyErrorSum = 6` / `eventCountDiff = 6` over 252 ground-truth
events — all six misses are `|dy| = 1` steps rejected by the detector's noise
gate by design. Note: the multi-frame transition-window **debounce is deferred
to M3** (with the `SessionSegmenter`), because without a multi-page fixture it
has no measurable exit criterion — implementing it in M2 would have violated
the tests-before-algorithms rule.

**New classes:** `VideoSource` (wraps `cv::VideoCapture`; yields
`{frame, index, t = index/fps}`; one place that knows about decoding),
`TransitionClassifier`.

**Algorithm draft — `TransitionClassifier::classify(prev, curr)`:**
- `IDENTICAL` if `mean(|prev−curr|) < 2.0` (exists as the pre-filter today).
- Else run `ScrollDetector`. If valid → `SCROLL(dy)`.
- Else compute the **changed-area fraction** of the interior (blurred absdiff >
  threshold, count / area) and the **fixed-band change** (same measure inside
  the current fixed-band estimate):
  - changed fraction high (≈ > 0.4) **and** no dy explains the interior
    (scroll histogram flat/invalid) → `PAGE_CHANGE` candidate.
  - fixed bands changed too (header pixels differ) → strong `PAGE_CHANGE`
    confirmation — chrome only changes on navigation.
  - otherwise → `UNSTABLE` (in-page animation, partial render, popup).
- **Debounce:** navigations are multi-frame (white flash, progressive render).
  A `PAGE_CHANGE` candidate opens a *transition window*; consume frames until
  N consecutive stable/scroll transitions, then close the window. Frames inside
  the window are excluded from stitching. *(Deferred to M3: single transitions
  are classified in M2; the windowing belongs to the `SessionSegmenter`, where
  a multi-page fixture makes it measurable.)*

**Scroll-event export:** each `SCROLL(dy)` becomes
`{t, frameIndex, dy, vt}` — collected by the caller and emitted (first cut of
the JSON report; schema grows in M6).

**Tests / exit criteria:**
- Benchmark gains a *scroll-event score* (per-event |dy| error, event count)
  against the recorder's ground-truth scroll log.
- On the existing single-page session: **zero** `PAGE_CHANGE` false positives;
  scroll-event trace matches ground truth within tolerance.
- Existing stitch benchmark stays ≤ 99 px.

---

## Milestone 3 — Session segmentation + per-page stitching ✅ (2026-07-03)

**Goal:** multiple pages per video; one stitched screenshot per page visit.

**Status: done.** `SessionProcessor` (session classify pass → segment → one
`PageStitcher` per visit), `SessionSegmenter` (with the debounced transition
window deferred from M2), `Stitcher` → range-scoped `PageStitcher`,
`FixedAccumulator` shared, report schema with `pages[]`/`navigations[]`/
per-event `pageId`; benchmark scores reports (no stdout parsing) and handles
both session types with per-session history. Measured exit criteria: multipage
totalError 82px — pageCount 2/2, **boundary error 0 frames** (criterion ±3),
page heights −2/0 px vs what each visit saw, scroll dyErrorSum 2px; single
session unchanged (99px). One algorithmic finding en route: a navigation
between same-chrome pages was invisible to the full-frame changed fraction
(0.39 < 0.4 gate — shared chrome dilutes it); fixed by feeding the session
pass an **online chrome estimate** (stability accumulated across scroll
transitions, reset at every page change), which lifts the fraction to the
interior-only value (0.67 measured).

**New classes:** `SessionSegmenter` (folds the transition stream into
`PageVisit {firstFrame, lastFrame, tEnter, tExit}` segments — including the
multi-frame **transition-window debounce** deferred from M2),
`SessionProcessor` (drives: segment → stitch each), and the refactor of
`Stitcher` → `PageStitcher` operating on a **frame range** of a `VideoSource`
instead of the whole file (the 3 passes stay, scoped to the segment; fixed
mask/rects are per visit).

**Design notes:**
- `PageStitcher` keeps its canvas + fixed rects + `vt_by_frame_` as *state*
  that a later visit can extend (prepares M4 merging).
- `main` output becomes one PNG per page (naming/manifest finalized in M6).

**Test fixture ✅ (2026-07-03):** `record_session.mjs --plan multipage` records
page 1 → small scroll → three link hovers (real `:hover` CSS in the video) →
CDP click on the CTA → page 2, ~2.5 s stay (`session_multipage/`, 157 frames,
5.2 s). Ground truth carries the multi-page schema: `pages[]`, per-frame
`{page, scrollY}` (`page: null` = transition frames), `events[]` (hover
ranges, click, pageLoaded) and `navigations[]`. Note: file:// navigation is
instant — the fixture's page change is a **hard 1-frame cut** (the sharpest
classifier case); slow progressive-render transitions still need a real-world
recording. The `single` plan was regression-checked byte-identical.
**First real-world screencasts enter the corpus here** (scored initially for
"no crash + plausible page count" until ground truth exists).

**Exit criteria:** on a 2-page recorded session — exactly 2 stitches; page
boundary detected within ±3 frames of ground truth; each page's stitch scores
comparably to the single-page benchmark; single-page benchmark unchanged.

---

## Milestone 4 — Page identity: REVISIT recognition + stitch merging ✅ (2026-07-03)

**Goal:** recognize a page the session has seen before; merge the revisit into
the existing stitch. Navigation events are classified as **`NAVIGATE`** (first
time this page is seen) or **`REVISIT`** (known page) — nothing more.

**Status: done.** `PageIdentifier` (frame-vs-canvas block voting) +
`PageRegistry` (visit assignment, merge); `PageStitcher` reworked to
multi-visit (`add_visit(visit, vt_start)` + merged `reconstruct()`). Fixture:
`--plan revisit` (page 1 → page 2 → **history back**, which restored the
scroll — the revisit enters mid-document at 588 px). Measured exit criteria
(totalError 166 px, all checks pass): 2 stitches for 3 visits; `nav1` =
REVISIT with **vtStart error 0 px** and boundary error 0; merged page-0
height −4 px vs everything both visits saw; zero false merges (multipage
session's page 2 vs page 1: below confidence threshold). Two findings en
route, both measured and fixed:
- **False scroll on the back-navigation** — periodic content let a bogus dy
  win the vote histogram, so the PAGE_CHANGE never fired. Fix: a `Scroll`
  classification must also *explain* the transition (shift-and-compare
  residual ≤ `SCROLL_RESIDUAL_MAX`; measured ≤ 0.04 for true scrolls vs 0.27
  for the false one).
- **1-px chrome banding** — a band under-detected by 1 px made every frame
  write a row of header chrome at a different document row. Fix:
  `INTERIOR_ROW_MARGIN` crops the stitched interior a few rows inside the
  bands; the header/footer stamps extend by the same margin from the
  shallowest/deepest frame (row-diff artifact metric 29.3 → 11.4 ≈ the
  single-session reference).
- Deviation: the dHash pre-filter was **not** implemented — with the session
  page counts we have, the full block match is the measurable path; add the
  pre-filter when a corpus with many pages makes it measurable.

**New classes:** `PageRegistry` (owns `PageRecord {id, fingerprint,
PageStitcher, visits[]}`), `PageIdentifier`.

**Algorithm draft — identifying the page after a PAGE_CHANGE:**
1. **Cheap pre-filter:** perceptual hash (e.g. dHash) of the stable
   post-transition frame's fixed-band region + interior top strip; compare
   against each known page's stored hashes → shortlist.
2. **Confirm by viewport-to-document matching:** reuse the ScrollDetector
   machinery, but match the candidate *frame interior* against the known
   page's *stitched canvas* over all document offsets (block-vote histogram,
   frame-vs-canvas instead of frame-vs-frame). A strong vote peak at offset
   `vt₀` ⇒ same page — **and** immediately gives the entry scroll position
   (browsers restore scroll on revisits, so a visit may start mid-document;
   matching must search the full document height, not just vt = 0).
3. Match → `REVISIT`: the visit *resumes* that page's `PageStitcher` at `vt₀`;
   its frames extend/refresh the existing canvas (they may add coverage the
   first visit never scrolled to). No match → `NAVIGATE`, new `PageRecord`.

**Known weak point (state it, don't hide it):** dynamic content — timestamps,
ads, badges — makes a recurring page look different on revisit. Matching is
threshold-based and reports a **confidence**; a low-confidence revisit falls
back to `NAVIGATE` (a duplicate stitch is a milder failure than merging two
different pages into one canvas).

**Exit criteria:** recorded session page 1 → page 2 → link back to page 1
(restored scroll): exactly **2** stitches; the second page-1 visit is labeled
`REVISIT` and its frames land at the correct vt in page 1's canvas (no double
content); zero false merges on a session of two *different-but-similar* pages.

---

## Milestone 5 — Smart-stitch quality: anti-ghosting & dynamic content ✅ (2026-07-04, first pass)

**Goal:** the "smart" in smart screenshot — no duplicated footers, no smeared
animations, no scrollbar streaks, on the real-world corpus, not just fixtures.

**Status: done for the fixture scope.** New hostile fixture
(`scroll_page_hostile.html` + `--plan hostile`): a continuously animating
region (sliding gradient + spinner) and a floating fixed button that appears
only after 300px of scrolling. Recorder captures a **chrome-hidden full-page
reference render** (`fullpage.png`; position:fixed elements are hidden for the
capture because `captureBeyondViewport` would render them once at viewport
coordinates — mid-image footer); the benchmark scores a **ghost metric**:
per-16px-block MAD vs the reference, locally aligned per block-row within
±12px (scroll drift is not ghosting), animated bboxes excluded. Journey
(measured at every step): baseline was catastrophic — the animation caused 7
false PAGE_CHANGEs and vetoed true scrolls (dyErrorSum 512). Fixes:
- **Scroll veto redesigned**: `scroll_explains` now measures, per 32px band,
  the fraction of *unshifted-changed* pixels still unexplained after the dy
  shift, and takes the **median** band. Chrome/whitespace drop out of the
  denominator (no mask needed); a bounded animated region only dirties its
  own bands. Measured: true scrolls ≤ 0.002, false scrolls 0.114/0.999.
  The veto also **rescues weak-vote true scrolls** (animation steals histogram
  votes, breaking the dominance gate): the histogram proposes, the veto decides.
  (A viewport-space dynamic-pixel mask was tried and rejected: animation moves
  through the viewport while scrolling; the union over pause positions poisons
  the detector.)
- **Conflict-frozen writes** in reconstruct: a stitched row is refreshed only
  if the new observation agrees (within ±2px drift); inexplicable conflicts =
  dynamic content, frozen at first observation. The animated region lands as
  ONE coherent instant; the floating button's smear collapses to 2–3 small
  remnants (1.13% bad blocks).
- **Interior margin 4→12px** + sidebar-replication trim: chrome **drop
  shadows** (6px box-shadow) blend with scrolling content next to
  under-detected bands — they smeared shadow lines at every scroll step and a
  replicated shadow row painted a dark column down the page.
Result: ghost madMean 3.84 / badBlocks 1.13% (tolerances 6.0 / 2%); friendly
sessions unregressed (99/82/166). **Remaining M5 work** (needs new fixtures):
scrollbar-thumb exclusion (thin-column sub-row granularity), conditionally-
fixed overlay extraction (stamp the button once instead of remnants), and
validation on the real-world corpus.

**Known ghosting sources & algorithm drafts:**
- **Conditionally-fixed elements** (sticky header that appears only after
  scrolling starts, cookie banners, floating buttons): today's fixed mask is
  all-or-nothing over the whole visit. Draft: track per-band *temporal
  intervals* of stability (when did these pixels become/stop being fixed);
  elements fixed for only part of the visit are stamped as overlays from a
  representative frame and *cut* from scroll content only during their active
  interval.
- **Latest-wins overwrites with slightly-off vt** (the classic multiple-footer
  smear): per-document-row **provenance + confidence** — prefer writes where
  the row was far from the viewport edge and the local scroll vote was strong;
  optionally reconcile rows observed many times (modal observation instead of
  the last one).
- **Dynamic in-page regions** (carousels, videos, animated ads): rows whose
  repeated observations disagree → mark *dynamic*, freeze one canonical
  observation instead of mixing frames.
- **Scrollbar exclusion:** the scrollbar thumb moves proportionally to
  `vt / docHeight` — neither fixed nor content. Detect the thin rightmost
  column whose motion correlates with vt and exclude it from the interior
  (fill from a frame where the thumb is elsewhere).

**Exit criteria:** new hostile fixture pages (sticky-on-scroll header, an
animated region, visible scrollbar) reconstruct without visible ghosting;
benchmark gains a perceptual diff score (e.g. block-MAD vs a ground-truth
full-page render) and it improves measurably on these fixtures **and** on the
real-world corpus.

---

## Milestone 6 — Session report: the full output contract ✅ (2026-07-04)

**Goal:** machine-readable output equivalent to the production algorithm's
contract — with **confidence fields** wherever the pixel version infers what
the DOM version could observe.

**Status: done.** The report (built up through M2–M5) now carries the full
contract with `"schema": 1`: video info, session transition tally, per-page
`stitch`/`width`/`docHeight`, `visits[]` (frame range, timestamps, `vtStart`),
`fixedRects` with **`uncertaintyPx`** (bands only ever over-detect; the true
edge lies within `[detected − uncertainty, detected]`), **`overlays[]`**
(interior viewport-fixed elements — an addition over the draft schema),
`navigations[]` with `type NAVIGATE|REVISIT` + `confidence` + `vtStart`, and
`scrollEvents[]` with `pageId`. Band pass-tolerances in the benchmark now
match the declared uncertainty (48px) instead of an aspirational 16px —
`totalError` still tracks raw errors so improvements stay visible. Exit
criterion met: `python3 test_stitch_session.py --all` scores every recorded
session strictly in one command — all four sessions PASS (99/82/166/88 px,
ghost 3.04, all scroll/segmentation/identity checks green).

**`SessionReport` (JSON) draft schema:**
```jsonc
{
  "video": {"path": "...", "fps": 30, "frames": 1234, "viewport": {"w":1280,"h":900}},
  "pages": [{
      "id": 0,
      "stitch": "out_page0.png",
      "docHeight": 3251,
      "fixedRects": [{"kind":"header","x":0,"y":0,"w":1280,"h":78,"uncertaintyPx":16}, ...],
      "visits": [{"tEnter": 0.0, "tExit": 8.4, "nav": "NAVIGATE"},
                 {"tEnter": 15.2, "tExit": 20.1, "nav": "REVISIT",
                  "vtStart": 1120, "confidence": 0.93}]
  }],
  "scrollEvents": [{"t": 0.43, "frame": 13, "pageId": 0, "dy": 8, "vt": 8}, ...],
  "navigationEvents": [{"t": 8.4, "from": 0, "to": 1, "type": "NAVIGATE"}, ...]
}
```

**Exit criteria:** `test_stitch_session.py` validates the report against the
recorder's ground truth end-to-end (pages, visits, nav types, scroll events,
fixed rects, stitch scores) — one command scores the *entire contract*.

---

## Milestone 7 — Real-world hardening ◐ (first pass 2026-07-04)

**First pass done, driven by the production corpus** (each change measured on
all 14 sessions):
- **Blank-visit filter**: white loading screens between navigations are not
  pages — dropped (rw2 became a single page matching production; rw1's white
  intro gone).
- **MAX_SCROLL 400 → 880** + `session_jumps` fixture: instant ~400px leaps
  (Page-Down style) are detected exactly (dyErrorSum 0). Measured envelope:
  jumps are reliable to ≈half the content window; the hard physical limit is
  `viewport − header − footer` (no shared visible pixels beyond), and
  page-wide periodic content erodes the practical limit well below it.
- **Texture gate in scroll voting**: near-uniform blocks all resolve to the
  scan's first minimum, forming a junk bin that outvotes real peaks.
- **The explains-veto is now always MASKLESS**: passing the pixel fixed-mask
  concentrated the changed-pixel evidence on periodic regions and let an
  alias dy hide a page change (measured on the revisit back-navigation).
  Chrome and whitespace already drop out of the unshifted-changed denominator
  by construction.

### Fixed-position video ads (dynamic noise) — 2026-07-04

rw1_newsportal carries a mega fixed-position VIDEO AD on the right. Its churn
was previously classified as a PAGE_CHANGE storm — 44 false page changes that
fractured the single article into fragments and buried the scroll. Now:

- **DynamicAccumulator**: a pixel that changes across many NON-scroll
  transitions is persistent viewport-anchored noise. The confirm threshold is
  deliberately HIGH (`DYNAMIC_CONFIRM_FRAMES = 25`) — a *scrolling* animation
  only dwells at one viewport position for a single reading pause (the M5
  lesson that killed the first dynamic-mask attempt), while a fixed ad
  accumulates for the whole session.
- The mask is used ONLY where noise-exclusion is sound: the page-change
  spread, the scroll veto's changed-pixel evidence, and page identification.
  It is NEVER used for detector sampling (M5/M7 lesson).
- **Two-pass classification**: pass 1 matures the mask (an ad's churn reads as
  PAGE_CHANGE until the mask confirms it — chicken-and-egg); pass 2 re-runs
  seeded with the mask, so the ad collapses to Identical from frame one. On
  rw1 this cut the storm from 44 page-changes to 5.
- **Region-verified page identity**: block votes propose the offset, then a
  grid over the frame↔canvas intersection verifies it. Confidence = fraction
  of near-identical blocks (`IDENT_MATCH_MAD = 9`, tighter than the loose
  voting threshold). Measured: a true revisit matches 100% of interior blocks
  at MAD<9; two DIFFERENT pages sharing one chrome template collide at only
  ~15%. This is robust to partial content change (a revisit with some images
  swapped) yet does not confuse sibling pages.

Result: the ad is recognized as noise (265764px), masked, and the scroll is
recovered THROUGH the storm — the core of the request. rw1 still splits into
2 pages (not 1) and reaches 4076 of 4737px: the news portal AUTO-REFRESHED its
articles mid-session, so the post-storm view shares almost no content with the
pre-storm view — recognizing "same page after a full content refresh" has no
pixel signal to lock onto and is left as open backlog.

### Reload-REVISIT recognition + blank-fill behind overlays — 2026-07-05

Two corpus-driven improvements (measured on all 14 sessions):

- **Reload REVISIT (rw1).** A news portal that AUTO-REFRESHES its articles
  mid-session was splitting into two pages: the post-reload scroll shared too
  little top-of-page content with the pre-reload canvas for the first-frame
  identifier to recognise it. The fix keys on **vote concentration** — the
  share of near-identical block votes that agree on ONE document offset. A
  reloaded page keeps its surviving blocks (headlines, images that did not
  change) clustered at a single offset (measured 0.5-0.7), while two DIFFERENT
  pages sharing one chrome template collide at many offsets and scatter (~0.34,
  even when a best-offset overlap fraction looks deceptively ~0.5). A REVISIT
  is now accepted when the overlap fraction clears the bar OR a large absolute
  cluster of near-identical blocks survives (`REVISIT_MIN_MATCH_BLOCKS`),
  provided the concentration clears `REVISIT_MIN_CONCENTRATION`. Voting itself
  was tightened to the near-identical bar (`IDENT_MATCH_MAD`) so the histogram
  stays clean. Result: rw1 is now a SINGLE page, 4735px vs the DOM system's
  4744 — a page-for-page match. revisit_noscroll (two sibling pages sharing a
  template) still correctly stays two pages.

- **Blank-fill behind overlays.** Viewport-fixed floating elements (a scroll-
  to-top button, a "Haben Sie Fragen?" chat widget) are masked out of the
  scroll interior and stamped once — but the content behind them was left
  blank wherever it sat under the element in every frame that wrote its
  document row (the conflict-freeze also refuses to fill a blank column,
  reading the gap as disagreement). A new post-pass recovers those pixels from
  any frame that shows the same document row at a viewport position clear of
  the overlays. And overlays are now collected across ALL visits of a page
  (not just the last) — a revisit merges several visits, and an element
  verified in one must stay masked for every visit's interior copy, else it is
  baked into the scroll content at each position it was seen. On the hostile
  fixture the ghost score improved (madMean 3.84 -> 2.75, badBlocks
  1.13% -> 0.00%). Known residual: a chat widget whose icon sits a few px
  outside its detected text-pill box is not fully covered and still repeats on
  rw3 (overlay-detection coverage, not the fill).

Corpus scores after this pass: rw3 is a page-for-page match with the DOM
production output (6524/956/4327 vs 6520/960/4320); rw2 within 4px; rw5
per-page heights within 8px. **Remaining backlog:** rw4's very deep page is
still truncated (5542 vs 11624 — fling steps beyond the overlap limit; needs
registry-level intra-page re-localization), and rw5 splits fewer steps than
production's high-sensitivity setting (soft comparison).

*(Correction, 2026-07-27: an earlier draft of this paragraph also claimed
"rw1's article stitch reaches 86% of the reference". That number was left over
from before the reload-REVISIT fix in this same section — after it, rw1 is a
single 4735px page against a 4737px production JPG. The 4744 in the manifest,
and rw1's reference generally, is a CAPTURED EXTENT rather than a document
height: the scrollbar thumb puts that document at ~13000px and the user never
scrolled that far.)*

### Intra-page re-localization — 2026-07-27

**The problem.** Scroll tracking is a chain of frame-to-frame deltas, and the
chain breaks when one step exceeds the shared-pixel window
(`viewport − header − footer`). Past that, two consecutive frames have NOTHING
in common and no frame-pair method can measure the jump — to a classifier
seeing two frames, a jump is indistinguishable from a navigation. Measured on
rw4_portal, whose deep page the user scrolled by **dragging the scrollbar
thumb** (4356px in one 3.89fps frame step): the page fractured into a truncated
5542px stitch plus a bogus second page, and the identifier scored the fragment
`confidence 0 conc 0 blocks 0` — literally zero overlap to match against.

**Two evidence sources, tried in that order.** A visit that matches no page is
now tested for being a CONTINUATION of the page the session was already on:

- **Content** — the jump landed in territory the page's canvas already covers,
  so the frame matches it outright. A match against the *previous* page with
  `confidence ≥ CONTINUATION_MIN_CONFIDENCE` is a jump, not a revisit: a jump
  re-shows literally the same pixels (measured 1.000 and 0.913 on the new
  fixture, 0.885 on rw3), while a page that RELOADED in place repaints most of
  the viewport (measured 0.317 on rw1's auto-refreshing news portal, which must
  stay a REVISIT — a reload is a new document version).
- **Scrollbar** — where the jump lands beyond the canvas there is no content to
  match, by construction, and the browser's own scrollbar is the only pixel
  signal left. `ScrollbarTracker` locates the bar structurally (a column band
  holding two gray levels whose darker run MOVES) and `ScrollbarMap` calibrates
  `vt = a·thumb_top + b` from the page's **own content-tracked scrolls** — so
  nothing about platform scrollbar metrics is assumed. The slope has a closed
  form, `a = viewport_h / thumb_h`, which doubles as the sanity gate a
  mis-detected column cannot pass. Measured on rw4: 83 anchors, max residual
  7.0px over vt 0..4612; R² 0.99999; the implied track top is 16.6 against a
  true 17.

**Thumb height is the page fingerprint.** It is a function of document height
alone, so it survives a zero-overlap jump — the one identity signal that does
(measured on rw4: 72px for the 11624px page vs 576px for the 1449px one, an
8.02:1 ratio against the documents' 8.02:1).

**Guards** (a false merge is the worst outcome this project can produce, so
each is measured): only the immediately preceding page may be continued; the
scrollbar map must be calibrated from that page's own scrolls and its thumb
fingerprint must match; and a scrollbar-derived offset is accepted **only where
content matching is blind by construction** — beyond the canvas, leaving less
overlap than the identifier needs to form a verdict. Inside the canvas the
identifier had evidence and said no, and that verdict wins.

**The contract holds.** A re-localized jump is reported as one large SCROLL
event inside ONE page — because that is what it was — and emits no navigation.
Report schema bumped 1 → 2 for `pages[].gaps` (document rows for which no
scrolling content was ever observed — skipped by a jump, or hidden behind a
viewport-fixed element at every reachable offset) and `visits[].vtUncertaintyPx` / `relocalized`: a jump SKIPS content,
pixels never captured cannot be reconstructed, and a schema-1 consumer was
entitled to assume every pixel of a stitch had been observed.

**Measured.** rw4: 3 pages → **2**; deep page 5542 → **11618** vs the DOM
system's 11624 (−6px), with the fragment landing within **+5px** of where the
DOM-based system placed it (verified by aligning both against the production
stitch) and 4446px declared unseen. rw3: the −4563px jump that was reported as
a bogus REVISIT is now a scroll event; 4 navigations → 3, geometry unchanged.
New fixture `--plan fling` (leaps of −2353px and +1647px into already-seen
territory): 1 page, 0 navigations, `dyErrorSum` **4000px → 1px**,
`eventCountDiff` 2 → 0. rw1, rw2, rw5 and all nine lab fixtures byte-identical.

**Deliberate non-decisions** (measured; do not re-litigate):
- The frames the segmenter drops in its page-change window are NOT recovered,
  though the map could place them. They are captured DURING the jump and are
  **torn**: localizing rw4's frame 150 band by band inside the DOM reference
  puts rows 60..149 at offset 5571 and rows 510..899 at 6983. Stitching such a
  frame would bake a torn band into the canvas; one dropped viewport is cheaper.
- The scrollbar path **cannot be exercised in the lab** — CDP screenshots carry
  no scrollbar (verified across all nine fixtures, whose right-edge modal value
  is page white in 100% of frames). rw4_portal is its only test. The fixture
  covers the content path instead.
- The slope is FITTED only when the anchors span ≥ `SB_MIN_CAL_SPREAD` of thumb
  travel, because a short baseline makes least squares worse than the closed
  form: extrapolating 5300px beyond the calibrated range errs +138px from 50px
  of travel, +25px from 74px, +10px from 213px, while the geometric slope alone
  errs −7..−34px regardless.
- Never score a scrollbar-derived docHeight against a production stitch unless
  the session reached the document bottom: rw1's and rw3-page2's references are
  captured extents, not document heights (the thumb puts those documents at
  ~13000 and ~4960px, and the scrollbar-predicted captured extents match the
  production JPGs to 1-2px).

### The scrollbar as chrome, and overlay verification on the element's own pixels — 2026-07-28

Two defects that the scrollbar work exposed, both closed.

**The scrollbar column was being stitched as content.** With the bar now located
exactly, it is excluded from the scrolling interior and painted from a
**thumb-free strip** — the per-pixel MAXIMUM of the band over every frame of
every visit, which is the track because the thumb is darker and always moves on,
while the never-moving arrow glyphs survive. Arrows are painted at both document
ends (like the header and footer bands) with clean track between. It is reported
as a fifth `fixedRects` kind, `scrollbar`, with `uncertaintyPx: 0` — unlike the
four content bands it is DETECTED rather than inferred from pixel stability, so
its extent is exact. Rationale for not rendering the thumb at all: it encodes the
scroll position, which is meaningless in document coordinates — the same reason
the recorder hides `position:fixed` elements for its full-page reference render.
Measured on rw4's deep page: the column was 92% blank white (an overlay bbox
covered it, so the blank-fill pass could never recover it) plus 120 rows of thumb
remnants; it is now clean track on 11602 of 11618 rows.

**Overlay verification compared the whole bbox.** A round element in a square box
has background corners — ~21% of the area for a disc — and the two verification
frames show DIFFERENT content behind it by construction, so the corners alone
could sink a genuine overlay. Measured on rw3: the blue chat-launcher disc beside
the "Haben Sie Fragen?" pill scored MAD 42 against a threshold of 18 and was
rejected, so it stayed baked into the scroll content at every position it was
seen (~6 copies down the page — the "overlay-detection coverage gap" left open on
2026-07-05). Verification now compares only the confirmed-fixed pixels inside the
box: the disc scores **1.00**, and genuine overlays across the corpus land at
0.04-3.2 instead of 0.5-13, a much wider margin from the threshold. Rejected
candidates are now logged under `-v` (rw3 correctly rejects a 315×771 sprawl at
MAD 30.8 and a 94×32 at 149.6).

### Per-row modal reconciliation: stop trusting the first sighting — 2026-07-28

**The rule that was wrong.** `reconstruct()` wrote each document row from the
FIRST frame that showed it and froze it there, refusing later frames that
disagreed. That was built in M5 to stop animation smearing, and for that it
works — but it trusts the *least* trustworthy observation. A page is still
SETTLING when a visit begins: banners are still up, late-loading content is
still shifting the layout. The first sighting is the one most likely to be
transient, and freezing it makes a transient permanent.

Two measured failures, both found by inspecting output against the DOM
reference rather than by any benchmark number:
- **session_cookie**: the dismissed cookie banner was baked in as a navy band
  slicing a text line and the top edge of an image. Present in the first 35
  frames of 110, absent in the remaining 75 — a clear minority, kept anyway.
- **rw3 page 2**: the first headline ("Ihr Weg zu verlässlichen Produktdaten im
  Internet") lost **12 of its 31 rows**, sliced horizontally. The cause was not
  animation: every source frame renders it perfectly. The document **reflowed by
  ~12px** while the visit's opening frames were captured, so the same content
  belongs to two different document rows, and the freeze kept the pre-reflow
  placement and refused the correction. The scrollbar thumb reads (17, 178) —
  pinned at the track top — throughout those frames, which rules out a missed
  scroll and proves it was the document that moved, not the viewport.

**The rule now.** Every observation of a row VOTES; the appearance most frames
agree on wins; ties go to the LATER frame, because a page settles as time
passes. Votes are compact row signatures (per-bucket column means,
`ROW_SIG_BINS`), so tracking `ROW_VOTE_SLOTS` candidate appearances per row costs
~2MB for an 11618-row page instead of a canvas copy each, and a
majority-of-k step handles rows with more appearances than slots.
`reconstruct()` becomes two passes: count, then write each row from its winner.
A continuously animating region has no majority — every observation differs —
and still collapses to ONE coherent instant, which is what the freeze achieved
and the reason it existed. `ROW_CONFLICT_MAD` and the ±2px `explainable()` drift
test are gone.

**Made measurable first** (tests-before-algorithms): `session_cookie` now
captures a **settled** full-page reference (`fullpage.png`, taken after the
session with the banner dismissed and chrome hidden — the recorder's hostile-only
capture was factored into `captureSettledReference`), and the ghost metric is
scored for any single-document session, multipage schema or not. That criterion
FAILS on the old rule and PASSES on the new one, which is the whole point of
adding it:

| session_cookie ghost | first-observation freeze | modal reconciliation |
|---|---|---|
| madMean (tol 6.0) | **6.14 FAIL** | **2.57** |
| madWorst | 194.18 | 4.32 |
| badBlocks (tol 2%) | **4.56% FAIL** | **0.00%** |

rw3 page 2's headline goes from 19 to 31 rows carrying ink, against production's
29 — the 29 correct rows plus a 2-row remnant of the pre-reflow placement.

**The vote is scoped to ONE VISIT**, and an earlier visit's rows are never
overwritten by a later one. Settling happens *within* a visit; differences
*between* visits are genuine content changes — a page that reloaded is a new
document version, not a noisy observation of the old one — so a per-row majority
across visits blends two versions into an incoherent canvas. This is not a
theoretical worry: voting across visits dropped rw1's reload-REVISIT vote
concentration from 0.526 to **0.429**, under the 0.5 bar, splitting that page in
two. Scoping the vote per visit restored it and then some — **0.590**, more
margin than the first-observation rule ever gave the suite's most fragile result.

### Saying what is known and what is not: schema 3 — 2026-07-29

An adversarial review of the contract found three places where it asserted more
than it knew. All three are closed.

**1. `docHeight` was a captured extent wearing the name of a document height.**
`max_vt_ + frame_h_` is how much document the frames COVERED. On a session that
stopped early those differ by thousands of pixels — rw1 captured 4736px of a
document the scrollbar puts near 13000 — and because `gaps[]` was empty, the
report read as complete coverage of a document it had seen a third of. The field
is now `capturedExtent`, which cannot be misread, and beside it:

- `documentHeight {value, uncertaintyPx, source}` — the document's real height,
  derived from the calibrated scrollbar map. The track geometry falls out of the
  map itself: `vt = 0` puts the thumb at `track_top = −b/a`, and the arrow
  buttons are near enough equal that `track_h = frame_h − 2·track_top`. Measured
  on rw4: track_top 16.6 against a true 17, track_h 896.7 against 897, height
  **11613±39 against the reference system's 11624**; on rw2 **3977±13 against
  3981**; on rw3's page 2 **4986±23** against an independently measured ~4960.
  `null` where no map could be calibrated.
- `reachedBottom` — true/false/**null**. rw3's page 2 now says `false`: the
  session saw 4327px of a ~4986px document, which the report previously implied
  it had covered entirely.
- `driftPx` — dead-reckoning drift MEASURED, not assumed: the worst disagreement
  between the accumulated offsets and the scrollbar's independent absolute
  reference among the anchors the map accepted (rw3 page0 14px, rw4 deep page
  10px, rw5 pages 1-2px). `null` where there is no independent reference.

Two guards were needed to keep the estimate honest, both found by measurement:
the slope must have been FITTED from real thumb travel — with the geometric
fallback, extrapolating the length of a track magnified the assumption to 15232px
for rw3's 960px single-viewport page — and a map still carrying the fallback slope
may not REFUSE contradicting anchors, because it has nothing to protect and
everything to learn (on rw1 eight anchors from a visit that never scrolled fixed a
slope of 13.28, and the map then rejected the deep visit's 222px of travel, whose
true slope is 14.85).

rw1 ends as an admitted unknown rather than an estimate: it is an auto-refreshing
news portal whose document LENGTH changes between reloads, so one linear map
cannot describe its three visits (residual 340px against a 24px bar) and the map
is refused. That is the correct outcome — `documentHeight: null`,
`reachedBottom: null` — and it is what the honesty fix was for: the report no
longer implies a complete capture, it says it does not know. Per-document-version
maps would be a new capability and need a fixture; not attempted.

**2. `scrollEvents[]` silently omitted frames.** It lists only frames where the
offset CHANGED, and only inside visits, so a consumer could not distinguish a
frame that did not move from a frame with no answer at all — measured, up to a
quarter of a real session (rw1 31 of 124 frames). Schema 3 adds `frames[]`: one
row per frame of the video with `{pageId, vt}`, or `null` plus the reason
(`transition` for a frame inside a page-change window, `blank` for one belonging
to a visit dropped as a loading screen).

**3. `vtUncertaintyPx` asserted exactness.** It took two values across the whole
real corpus: 0 on 18 of 20 visits, and 40 once. `driftPx` above now supplies the
measured counterpart wherever an independent reference exists.

### The hot loop: patch_mad without OpenCV call overhead — 2026-07-29

`patch_mad` is the inner loop of both the scroll detector (up to 2·MAX_SCROLL
calls per sampled block per transition) and the page identifier (one call per
canvas row per sampled block). It was `cv::absdiff` + `cv::mean`, and at 16×16
the OpenCV overhead — a temporary, two kernel dispatches, and a `cv::Mat` header
built per candidate position — dwarfs the arithmetic completely.

Measured with a standalone benchmark of one identifier vote stage (120 blocks
scanned down an 11618-row canvas): **322ms** for the OpenCV form against
**12ms** for a raw pointer loop — **27×**, for the same result. Bit-identical, not
just close: both sum exactly (a 16×16 patch sums to at most 65280) and the
quotient k/256 is exactly representable in float. A second helper,
`patch_mad_at`, compares a patch against a window of a larger image without
building a header for the window at all.

End to end this is 3-37% per session, not 27×, because video decoding and the
reconstruct passes are untouched — it means `patch_mad` was roughly a third of
total runtime and is now negligible:

| session | before | after | |
|---|---|---|---|
| session_page1 | 24.8s | 15.7s | −37% |
| session_hostile | 14.3s | 9.5s | −34% |
| session_fling | 25.4s | 17.7s | −31% |
| session_revisit | 13.7s | 9.5s | −31% |
| rw1_newsportal | 11.7s | 9.0s | −23% |
| rw4_portal | 21.6s | 19.0s | −12% |
| rw5_mobileframe | 30.9s | 28.6s | −8% |

All 16 stitched PNGs verified byte-identical to the pre-optimization hashes.

This was NOT speculative: the measurement came first, from asking whether an
index over page canvases would pay off. It would not have — the identifier's cost
was constant-factor overhead, not algorithmic. With the overhead gone, matching a
frame against every known page costs ~12ms per page instead of ~322ms, which
moves the page count at which a shortlist index becomes necessary from about 3 to
about 80. The dHash pre-filter therefore stays deferred (M4's reasoning holds:
the corpus has at most 5 pages per session, so an index cannot be measured yet).
When it IS measurable, two index keys already exist in the codebase: the
scrollbar thumb height (a document-height fingerprint, invariant to scroll
position) and the per-row signatures from the modal reconciliation.

### Incremental folding: the canvas is built once, not once per visit — 2026-07-29

`reconstruct()` is called by the registry after **every** visit, and it rebuilt
the whole page each time — re-reading every earlier visit. Cost therefore grew as
`V(V+1)/2` in the visits per page, which is invisible on the fixtures (1-4
visits) and fatal for a long session.

Now the scroll content, its written map and the count of folded visits are
**persistent state** of the `PageStitcher`, and a call folds only the visits it
has not seen. A fold is valid only while the geometry it was made under holds, so
a change of the interior rect or of the overlay set triggers a full refold — both
are inputs to every write, deciding which columns get copied at all. Growth of
the document is not a geometry change: the buffer is extended and existing rows
keep their document coordinates.

The chrome is deliberately NOT part of the persistent buffer. The footer,
bottom-anchored overlays and the scrollbar's lower arrow are positioned from the
document BOTTOM, so their stamp locations move whenever the document grows;
keeping them out means a growing page cannot leave stale chrome stranded
mid-document. Each call composes `content_ + stamps` into the returned canvas.

Measured with a new fixture, `--plan manyjumps` — eleven visits on ONE page
(leap between the document's two ends ten times, each leap re-localized as a
continuation):

| session | visits | before | after | |
|---|---|---|---|---|
| session_manyjumps | 11 | 59.5s | **31.3s** | **−47%** |
| rw5_mobileframe | 8 | 35.9s | 30.9s | −14% |
| session_fling | 3 | 26.6s | 25.4s | −4% |
| the other 13 | 1-4 | — | — | within ±2.6% (noise) |

Verified by hashing the stitched PNG of all 16 sessions before and after: **every
one is byte-identical**. That is the whole point — the previous rule was already
per-visit with earlier-visit-wins, so folding visit by visit is the same
computation with the redundant re-reads removed, and the hash comparison is what
proves it rather than argues it.

Cost: peak RSS rises 40-80MB (the persistent content buffer sits alongside the
composed output — measured rw4 439 → 502MB). Accepted: the composition is only
needed when someone asks for the image, so an online consumer would allocate it
on demand rather than hold it.

**How often the fast path actually applies**, from the `[page N fold]` verbose
line: `manyjumps` folds 10 of its 11 visits incrementally (its geometry settles
after the first), rw5 gets 3 incremental folds on page 0 — but on **rw3 and rw4
every multi-visit fold is a full refold**, because their fixed bands or overlay
sets change between visits. That is why those two barely moved while manyjumps
halved. The trigger is deliberately conservative: it refolds on ANY change of the
interior rect or the overlay set, since both decide which columns a write copies
at all. Loosening it (an overlay set that only GREW, an interior that only
shrank) needs its own correctness argument per case and is not attempted here.

**A fragility the scrollbar work exposed, and the next thing worth fixing.** The scrollbar was
first also excluded from the page identifier's sampling rect — principled, since
the thumb tracks the scroll position and says nothing about WHICH page a frame
shows. That cost rw5 a duplicate page (5 → 6) and the manifest guard caught it.
Cause: `PageIdentifier` draws its sample blocks from a seeded RNG over the
sampling rect, so **narrowing the rect by 15 of 1918 columns redraws every
sample** and moves confidence and concentration by a few percent. rw5 sits on a
knife edge — three visits at concentration 0.46-0.474 against
`REVISIT_MIN_CONCENTRATION = 0.5` — and one tipped. The exclusion was reverted
(its benefit is unmeasurable, the regression was measured) and the reasoning is
recorded at the switch in `page_registry.cpp`.

The real defect is the sample-dependence: identity verdicts should not move when
the sampling geometry shifts slightly. A deterministic GRID over the rect instead
of an RNG draw would make confidence and concentration stable and reproducible
under exactly this kind of change. It would also move every session's numbers at
once, so it needs its own measured pass — and rw5's 0.46-vs-0.5 margin should be
understood before the thresholds are touched, not after.

Second instance, measured 2026-07-30 while landing the noise-rate work: on a
16-page desktop session outside the committed corpus, one visit's verdict flipped
from REVISIT to a new page on `concentration 0.516949 → 0.483051` — a 0.034
wobble across `REVISIT_MIN_CONCENTRATION` — while its confidence barely moved
(0.940 → 0.944) and its matched-block count went UP (125 → 136). Nothing about
that page changed; the visit boundaries were identical; the samples were redrawn.

**Superseded later the same day**: a seed sweep showed this is worse than
instability — the shipped rw5 baseline is a seed artefact, and a deterministic
grid makes it worse rather than better. See *The embedded screen flow: why its
pages are unusable* below for the measurements and for what the fix now has to be.

Side observation, deliberately NOT acted on: while a scrollbar is present a
genuine fixed right SIDEBAR can no longer be detected, because `right_w_` counts
contiguous fully-fixed columns inward from the edge and the bar's moving thumb
stops that scan at the first column. No session in the corpus has both, so there
is nothing to measure a fix against.

### In-place change is not an ad: a rate, not a count — 2026-07-30

Four new screen-flow recordings — a phone screen embedded in a survey page, the
participant tapping through it — came out almost empty. All four confirmed
175k-195k px of "persistent noise", and one of them then reported
`identical 1245/1258, scroll 0, pageChange 3`: not a single scroll in 1258
frames. The session's entire content — the thing being studied — had been
classified as a video ad and erased from the record.

The persistent-noise mask was built for one real case, rw1's fixed video ad, and
its rule was an absolute COUNT: a pixel that changed during 25 non-scroll
transitions is noise. That rule cannot tell "changes often" from "the session is
long". A 2000-frame session in which somebody taps through 40 screens crosses 25
just as surely as a 124-frame session with an ad churning every frame — and the
regions it then swallowed are exactly the ones this project most needs to see: a
gallery, a tab switcher, a subframe, an embedded phone screen. Punctuated
in-place change has an ad's per-pixel signature and none of its persistence.

**The fix is a rate.** A pixel is noise only if it changed in more than
`DYNAMIC_MIN_CHANGE_RATE` (0.10) of the non-scroll transitions it was observed
in, with the old count kept as an absolute floor. Measured per-pixel change rate
inside the region the count alone confirmed as noise:

| region the count called "persistent noise" | median change rate | p90 |
|---|---|---|
| rw1 — genuine fixed video ad | **23.6%** | 26.8% |
| embedded phone-screen flow, session 1 | 3.9% | 5.2% |
| embedded phone-screen flow, sessions 2 / 3 | 1.6% / 1.9% | ≤ 3.1% |

10% sits in the empty middle of a 6-15× gap. The rate also serves the mask's
ORIGINAL intent better than the count did: the M5 lesson that killed the first
dynamic mask was that a *scrolling* animated element accumulates only for the
length of one reading pause (≤ ~18 frames) before it moves on — which is 4.5% of
a 400-frame session, correctly not noise, and it stays correct however long the
session runs. Seeded pixels (pass 2 of `classify_transitions`) stay confirmed
regardless of rate: a completed first pass established them, and re-deriving them
from the second pass's partial counts would un-confirm them for its early frames.

rw1 is unchanged by this — still one page, still 4736px captured, the ad still
masked, same overlay rect, same transition tally — which was the constraint the
change had to respect.

**What it did to the recordings that prompted it.** Measured over 28 sessions of
new material (two embedded-phone-screen studies, two desktop studies), before →
after:

| | pages per session | page-change transitions | scroll transitions |
|---|---|---|---|
| phone-screen study A (8 sessions) | 1-7 → **7-9** | 3-15 → 21-29 | 1-10 → 7-30 |
| phone-screen study B (5 sessions) | 2 → **4-11** | 3 → 21-36 | **0 → 7-106** |
| desktop studies (15 sessions) | unchanged in 12 of 15 | ±0-2 | ±0-9 |

The DOM-based production system produced about 8 screenshots per session on the
first study and 5 on the second, so the new page counts are in the right
neighbourhood rather than merely larger. The second study is the sharper result:
every one of its sessions had reported ZERO scroll transitions, because the mask
covered the region that was actually moving.

It costs ~3× runtime and ~2× peak RSS on those sessions — it is now stitching
content it used to discard. The desktop sessions never confirmed a noise mask at
all, so the rate gate cannot touch them; 12 of the 15 are identical page for page
and pixel for pixel in captured extent. The three that moved were traced
individually, because "12 of 15 unchanged" is not an argument about the other
three:

- one lost a spurious 4-frame visit, after which the next visit was recognised as
  a CONTINUATION of page 0 (confidence 1) instead of becoming a new page — 4 pages
  → 3, an improvement, caused by the veto gate;
- one had a visit split at a transition the veto now classifies differently; its
  two halves identify separately and the session's captured extent grows by 360px;
- one is the `PageIdentifier` sampling knife edge with identical visit boundaries
  (see the sample-dependence note above) — nothing to do with either gate.

**A lab fixture, and the second bug it found.** `--plan gallery` taps an embedded
320×260 panel 30 times with 18-frame dwells (5.5% rate, 30 > 25 count), from a
scrolled position, with the panel ~700px down the document. On the previous code the panel was confirmed
as noise and stamped once as a viewport-fixed overlay — and the fixture also
truncated to 1457px of a 1971px document, which was not the noise mask at all:

> On every screen-1→2 tap the scroll search proposed **dy = −320** and the veto
> ACCEPTED it. The veto measures, per 32px band, how much of what changed is left
> unexplained by the shift, and takes the median. Under dy = −320 the panel's
> newly dark body landed on the same-shade page header 320px above, so two of the
> three bands with any evidence came back perfectly explained — median 0.000, on
> all ten repetitions. The other 15 bands had not changed and were treated as
> neutral.

They are not neutral. Content moves through *every* band, so a textured band that
stayed put contradicts a nonzero dy. The veto now also requires
`SCROLL_MIN_CHANGED_BAND_FRAC` (0.30) of the informative bands to have changed at
all. Measured over 1232 true lab scrolls the worst case is 0.393 (at |dy| ≤ 4px)
with medians 0.64-0.87; over 213 true scrolls in the real corpus the worst is
0.545 with medians 0.82-1.00; the false panel swap sits at 0.167. The same gate
declines a genuine scroll inside a SUBFRAME, which changes only its own bands —
correct, because an inner scroll is not a document scroll and counting it as one
corrupts every offset after it.

With both gates the fixture is exact: `capturedExtent` 1971 of 1971, `dyErrorSum`
**0px**, 176/176 scroll events, one page, zero navigations, no overlay claimed on
the panel, ghost madMean 3.07 / badBlocks 0.98%. It fails on the previous code in
three independent ways (height 1457, panel claimed fixed, ghost madMean 64.7), and
still fails with the veto gate alone (panel claimed fixed) — so it guards both
axes, not just the one that happened to be measured last.

### The embedded screen flow: why its pages are unusable — 2026-07-30

The rate gate above stopped the phone-flow studies being erased, but the pages it
now produces are still not usable: one canvas shows several app screens
superimposed. Diagnosed on one session (424 frames, 4 pages), and the cause is
not where it looks.

**The visit that produces the mush contains thirteen different app screens.**
Extracting one frame per dwell shows the participant walking a whole onboarding
flow — bank search, bank selected, login form, password, account list, consent
screen, consent toggled, "card being created", "you're all set" — thirteen states
inside one visit, at seven document offsets, the states at one offset overwriting
each other.

**All thirteen boundaries were classified IDENTICAL.** Instrumenting the
classifier gives the reason immediately: the first gate is a frame-wide mean
absolute difference against `IDENTICAL_FRAME_MAD` (2.0), and the phone screen is
357×555 px of a 1678×1020 frame — 11.6% of the area, mostly white with sparse
text. A complete screen swap moves that mean to **0.87-3.47**. Measured on the
thirteen boundaries: ten fell below 2.0 and were discarded by the mean gate; the
other three were discarded by the second gate, which is the same frame-wide mean
computed with the noise mask zeroed (masked means 0.94-1.71). The two real
navigations either side of the visit measure 10.2-13.6 — that is the only reason
they fire. **A frame-wide average cannot see a total change of a sub-region**, and
that is the general bug: it applies equally to a gallery, a tab switcher, a
subframe and an embedded prototype.

The obvious replacement — count changed pixels instead of averaging their
magnitude — is not usable as-is, and the measurement says why. Changed-pixel
counts (3×3 blurred absdiff over `SAD_THRESHOLD`):

| | changed px |
|---|---|
| true duplicate frames (lab and real) | **0** (p25 = 0 on every real recording) |
| cursor moves, caret blinks, hover highlights | 100-5000 |
| the 13 screen swaps | 10112-74630 |
| **rw1 ad churn OUTSIDE the confirmed noise mask** | **10432-60220** (median 36060) |

The ad's residual churn and the screen swaps occupy the same range, so any
threshold either keeps erasing the screen flows or re-opens rw1's page-change
storm. Nor is there a temporal separator: per-pixel change rate (ad 19.5% vs
phone 10.6%), longest run of consecutive changes (6 vs 4), and longest still run
(58 vs 60) all overlap, and a lab animation that must NOT be masked sits between
them on every one of the three. **An ad and an app screen flow are genuinely
indistinguishable as pixel-temporal processes.** What separates them is what
fraction of the ACTIVE area they occupy — and the active area is exactly what the
online chrome estimate would supply, except that it is blanket-reset at every page
change, so on a session with 32 in-place page changes it never matures (measured
`fixedPx=1529752` immediately before a page change, `0` for the whole visit
after). That is the thread to pull, and it is the same adaptive-denominator idea
the corpus has been asking for since the no-scroll sessions.

**A second, independent defect — and it is fixed by one constant.** Even leaving
the states in one page, the canvas was blending them *within* each row band. The
cause is not the vote's slot count (raising `ROW_VOTE_SLOTS` from 3 to 24 changed
the output by 2.1 MAD — nothing): it is the row SIGNATURE. Sixteen column-mean
bins with a 12.0 tolerance cannot tell two mostly-white text rows apart, so
visually different appearances merge into one candidate and the elected exemplar
is whichever frame arrived last in that merged group — a different frame for each
row, which reads as a double exposure. At 64 bins with a 3.0 tolerance the
appearances separate. Measured, with no change to any other fixture:

| fixture | 16 bins / 12.0 | 64 bins / 3.0 |
|---|---|---|
| session_gallery ghost | madMean 3.07, **madWorst 83.06**, badBlocks **0.98%** | 2.58, **5.66**, **0.00%** |
| session_cookie ghost | 2.57 / 4.32 / 0.00% | unchanged |
| session_hostile ghost | 2.77 / 13.35 / 0.00% | unchanged |
| session_page1 totalError | 106px | unchanged |

The 83-MAD worst block was the same defect inside the gallery fixture built one
day earlier — in-place change blended rather than elected. So this is a real
quality fix with a measured win and no lab cost.

**It was blocked, and the blocker was worth more than the fix.** Sharpening the
canvas changes it, which redrew `PageIdentifier`'s RNG samples, which flipped rw5
from 5 pages to 6 — the fourth time that same knife edge had moved. So the
sample-dependence was measured properly, by sweeping the seed:

| samples | seed 42 | seed 7 | seed 99 | seed 1234 | seed 555 |
|---|---|---|---|---|---|
| 120 (shipped) | **0.516 → 5 pages** | 0.443 → 6 | 0.442 → 7 | 0.402 → 6 | 0.459 → 6 |
| 360 | 0.446 → 6 | 0.462 → 6 | 0.446 → 6 | 0.373 → 7 | 0.431 → 6 |

rw5's concentration for that visit is **0.44 ± 0.04**, comfortably below
`REVISIT_MIN_CONCENTRATION` = 0.5. The 0.516 that produces the committed 5-page
baseline is an outlier of seed 42, and tripling the sample count does not rescue
it — it confirms it. **The corpus expectation `rw5 → 5 pages` is calibrated to a
lucky seed**, and so, to an unknown degree, is every other verdict sitting within
a few percent of that threshold (rw1's reload merge at 0.526-0.590 and the
fling fixture's continuation confidence are the two known others).

Two sampling schemes were tried and both failed, informatively:

- A deterministic GRID (the fix this document has recommended for three
  sessions) **aliases with a page's own periodic layout**: samples land on evenly
  spaced cards and sidebar rows, their votes agree on offsets that are multiples
  of the layout period, and the concentration of a template collision rises. It
  merged the two sibling fixture pages of `session_revisit_noscroll` — different
  documents sharing one chrome template — into a single page, which is the exact
  false merge concentration exists to prevent.
- Keeping the draw random but anchoring the SEQUENCE to the frame instead of the
  interior rect (so a rect change only adds or removes samples at its edge) fixed
  rw1, rw5 and revisit_noscroll, and broke `session_fling` and
  `session_manyjumps` — their continuations dropped below
  `CONTINUATION_MIN_CONFIDENCE`, one more decision revealed to be knife-edge.

So the order of work was forced, and it was not a refactor but a
**re-derivation**: measure the distribution of concentration and confidence over
seeds for every decision in the corpus, choose thresholds with real margins (and a
sample count that makes the margins meaningful), and re-baseline the manifest
where its expectation turned out to encode a coin toss. That is the next section;
with it done, the row-signature fix landed unchanged and its only effect on the
suite is the gallery ghost improvement in the table above.

### Page identity, re-derived from measured distributions — 2026-07-30

The section above ends with "an improvement anywhere upstream of the canvas cannot
be told apart from a regression in page identity". This is the pass that fixes
that, and it is a re-derivation rather than a refactor: the algorithm is
unchanged, the two numbers it is judged by are now measured instead of assumed.

**What was actually wrong.** `concentration` is a vote PROPORTION, so its noise
falls only as 1/sqrt(samples) — at `IDENT_SAMPLES = 120` the deciding vote carried
a standard deviation of 0.03-0.08. Sweeping the RNG seed over the twelve
identity-bearing sessions at that setting:

| session | page counts across 8 seeds |
|---|---|
| rw1_newsportal | **1** (5 seeds) / 2 (3 seeds) |
| rw5_mobileframe | 5 (seed 42 only) / **6** (6 seeds) / 7 (1 seed) |
| session_fling | **1** (6 seeds) / 2 (2 seeds) |
| session_manyjumps | **1** (5 seeds) / 2 (3 seeds) |

Four of twelve, including both re-localization fixtures. The shipped verdicts were
partly a property of seed 42. The observed spreads track the binomial prediction
`sqrt(p(1-p)/votes)` closely, so this is sample noise and nothing subtler.

**Sample size.** `IDENT_SAMPLES` 120 → **480**. Ten of the twelve sessions become
seed-stable on that alone, and it costs 5% of the deepest session's runtime
(rw4 19.0s → 20.1s) — affordable only because `patch_mad` made the vote loop 27×
cheaper in July.

**Threshold.** With every decision labelled from ground truth (lab fixtures) or the
manifest (rw1-rw4), the corpus draws one narrow gap at 480 samples:

| | decision | concentration |
|---|---|---|
| lowest that MUST match | rw1's reload-revisit | **0.514 ± 0.030** |
| highest that must NOT | two sibling pages sharing a chrome template | **0.416 ± 0.027** |

`REVISIT_MIN_CONCENTRATION` was 0.5, taken in July from rw1's then-measured 0.526
— i.e. the bar sat directly on top of the single case it was fitted to, leaving
**+0.5σ** on the merge that must happen against −3.1σ on the merge that must not.
Centring it in the gap at **0.465** balances them at 1.6σ / 1.8σ. That is the best
this statistic can do: the gap is only 0.098 wide.

**A sharper statistic was tried and rejected on measurement.** Peak-vs-runner-up
dominance (does one offset dominate the vote histogram, rather than hold a share
of it) should in principle separate "same document, reloaded" from "different
document, same template" — but measured over 4 seeds at 480 samples it separates
them WORSE: rw1's reload 3.39 ± 0.74 against the sibling collision 3.29 ± 0.25,
overlapping, where concentration keeps a 25% relative gap. Anything better needs
evidence the vote histogram does not carry.

**Result: every one of the twelve identity-bearing sessions now produces the same
page and navigation structure under all eight seeds** (was eight of twelve), with
all 17 suite sessions PASS and every fixture at its exact previous `totalError`
and ghost score.

**rw5's expectation was calibrated to that lucky seed and is now a range.** Its
deciding visit sits at 0.420 ± 0.022, about 2σ BELOW the new bar, so the page
count is reliably 6 — while a second visit ([306..326], 0.552 ± 0.037) sits about
2σ above it. A session with visits on both sides of the bar has no single right
count, and pinning one would only record which side a coin toss landed on. The
manifest now expects `pages: [6, 7]` plus a `pageHeightsPresent` guard — every
distinct document extent the session is known to contain (including the tall 1880
and 3824 stitches production also captured) must still be reconstructed by some
page. That keeps the guard that caught rw4's truncation without asserting which
visit founds which page.

Worth stating plainly, because it is the lesson: a threshold fitted to one
measurement of one case is not calibrated, and the only way to find out is to
measure the same decision many times under different sampling.

### The active area: what part of the viewport can change at all — 2026-07-30

Two changes, both aimed at the same blind spot: the classifier was normalising
against the whole frame, and the estimate that would tell it better was being
thrown away.

**The chrome estimate survives a page change.** It used to be blanket-reset there,
which discards the evidence that a shared template did NOT change. Feeding the
page-change transition to the accumulator like any other transition is strictly
more informative — pixels that changed reset to 0, pixels that did not keep
counting. It matters most on the sessions that never scroll: measured on the
embedded-prototype recording, 1529752 px of confirmed chrome immediately before a
page change and **0 for the whole visit after**, so the classifier spent the rest
of the session normalising against the whole frame.

**The IDENTICAL gate is measured over the active area.** A frame-wide mean cannot
see a total change of a sub-region — the whole point of the previous section. The
mean is now taken over the frame minus what carries no change signal (confirmed
chrome, confirmed persistent noise), with a floor: if that leaves less than
`IDENTICAL_MIN_ACTIVE_FRAC` of the frame the estimate is not trusted and the full
frame is used. This also folds the old "all change inside noise ⇒ Identical" rule
into the same expression instead of keeping it as a separate special case.

**Result on the case that prompted it.** The visit that held thirteen app screens
splits: 8 visits → 19, page changes 21 → 46, and the phone screen in the stitched
page goes from layered grey ghosts to **one legible screen** — headline, body text,
the "Online-Banking Login" block and the red button all readable. The suite is
unmoved: all 17 sessions PASS with every `totalError`, ghost score and
`dyErrorSum` byte-identical to the previous run, because on recordings whose chrome
estimate already matured nothing about the arithmetic changed.

**The cost, measured rather than discovered later.** More visits means more
identity decisions, and each one is another chance to land near the bar. Re-running
the 8-seed sweep:

| session | before | after |
|---|---|---|
| rw1_newsportal | 1 page, 8/8 seeds | 1 page, **7/8** (its deciding visit now starts at frame 67 instead of 62, and that frame is a weaker match: 0.514 → 0.481 against a 0.465 bar) |
| rw5_mobileframe | 6 pages, 8/8 | **5 / 6 / 7** (its later decisions cascade — one visit's verdict swings ±0.225 because the canvas it is compared against depends on earlier verdicts) |
| the other ten | stable | stable |

So the seed-robustness won in the previous section is partly spent here. It is
spent on something real — a session whose content was unusable is now usable — and
the two remaining unstable sessions are the two the corpus already documents as
hard. It is recorded here rather than smoothed over.

**The obvious repair was tried twice and does not work yet.** If one frame is thin
evidence, identify a visit from its first few frames. Both aggregators break
something:

- **median of 3** (a marginal verdict must convince a majority of the frames):
  rw1's reload-revisit drops below the bar → 2 pages, and rw4 fails its height
  window.
- **best of 3** (does any settled view recognise the page): rw1 is fixed and the
  sibling-page collision still correctly refuses, but rw4's deep page truncates
  from **11618px to 5542px** — the extra frames turn one of its continuations into
  a revisit, and the re-localization that recovers the fling is lost.

Both were measured on the corpus rather than reasoned about, and both are wrong
for the same reason: the entry frames of a visit are not interchangeable evidence
about the same thing. Which one to use depends on whether the question is "did
this page reload" or "did the viewport jump inside it" — and those are decided by
different thresholds today. `IDENT_ENTRY_FRAMES` is left at 1 until that is
understood.

### Subframes: a changed region is reported, not resolved — 2026-07-30

The previous sections end at a wall: an embedded screen flow and a playing video
ad are the same event to a document model — "the page did not move but some of it
is different" — and four different pixel-temporal statistics fail to separate
them. What breaks the wall is a rule about behaviour rather than pixels, proposed
after seeing the gallery fixture: **if the region settles and holds, its states are
content; if it never holds still, it is noise.** Measured, per region, as the share
of in-place changes followed by a dwell of at least four transitions:

| region | in-place changes | dwell median | settled |
|---|---|---|---|
| lab fixture, animated panel | 300 | **0** | **0%** |
| corpus, fixed video ad | 18 | **0** | **0%** |
| real embedded phone screen (2 sessions) | 93 / 116 | 0 / 0 | **31% / 39%** |
| lab fixture, panel tapped 30 times | 30 | 17 | **100%** |

(The flows' dwell medians are 0 because a tap animates over two or three frames
before it lands; what matters is that a third of their changes are followed by a
real pause, and none of the ad's are.)

**The two lab fixtures differ in nothing else.** `--plan gallery` taps the panel 30
times; `--plan gallery_video` loads the same page with `#video`, which turns the
same 320×260 panel at the same position into a canvas that repaints every frame.
Same document, same geometry, same plan shape — one axis. The video plan starts the
animation on the button rather than on load, deliberately: scrolling past something
that is animating is a DIFFERENT problem with its own fixture (the M5 lesson — the
noise mask lives in viewport space and cannot follow a region that moves), and
measured on a version that played while scrolling, the host document fractured into
3 pages instead of 1 and lost 7 scroll events. That is a real limitation, recorded
here, not something this fixture should be re-testing.

**What the tracker reports** (`src/subframe_tracker.*`, one pass, no extra decode):
regions that changed while the document did not, each with its rectangle in frame
coordinates, its in-place change count, and — if it settled at least three times —
one image per settled state, written as its own file. A state's image is the whole
subframe rectangle, not the box that happened to repaint: one tap replaces all of a
panel and the next only its body, so cropping what changed hands back inconsistent
fragments of the same region. Detection details that cost measurements to get right:

- **Components, not a bounding box.** A bounding box spans everything that changed
  at once, so a screen swap that also repaints a button 1200px away reads as one
  1550px-wide region — measured on a real recording, where it merged an embedded
  phone screen with the host page's buttons.
- **Grouping by overlap RATIO, not by touching.** A chain of touching boxes
  swallows the viewport: measured, one group grew to 1800×956 of a 1918×956 frame.
- **A size floor.** Without it, every text block of a page that reloads its own
  content qualifies: 79 candidate regions of 0.3-3% of the frame on the
  auto-refreshing news portal. The regions this is for are 7-18%.
- **Attribution by containment.** By overlap ratio, an embedded phone screen kept
  2 of its 29 settled states, because its changes have wildly different shapes.
- **Three states, not two.** The hostile fixture's animated region happens to hold
  still twice in 204 frames.

**What it produces on the corpus.** rw1's fixed video ad: 151 in-place changes,
**0 settled states, noise** — and its auto-refreshing article teasers: 4 and 7
settled states, reported as screen flows, which is what that page does. rw2 and
rw4 (plain scrolling documents): no subframes at all. rw5's phone mockup: **13
states**, against the 19 stitches production emitted for it. The embedded-prototype
recording that started all of this: **17 states**, each individually legible — home
screen, Wallet, NFC payment, bank login, registration, TAN confirmation, video
identification, consent screen, "card being created", Google Pay, card ready.

**Schema 4** adds `subframes[]`. Position is given in frame coordinates because
that is what a consumer needs to lay a state back over the page it came from, and a
state's image is exactly that rectangle, so no scaling is implied. Nothing else in
the report changed, no existing behaviour changed, and all 18 sessions PASS with
every previous `totalError` and ghost score identical.

**Not done, and next:** the states of a subframe that SCROLLS internally are still
single frames, not a stitch. The pipeline already has everything needed to run on a
sub-rectangle's frame stream — transitions, offsets, reconciliation — so the honest
next step is recursion: verty on the subframe, producing one tall stitch per state
that scrolled. That is also what would finally give the embedded-phone case its
full-page screens rather than viewport-sized ones.

### Recursion: verty inside a subframe — 2026-07-31

A subframe that scrolls its own content is a document, and everything needed to
reconstruct a document was already written — classify its transitions, track its
offset, reconcile its rows, stitch it. The only thing in the way was that all of
it assumed "frame" meant the whole frame. It does not have to: `VideoSource` now
takes a rectangle, so the same `TransitionClassifier` and the same `PageStitcher`
read a smaller frame and mean the same things one level down. `SubframeStitcher`
is the bookkeeping around that, and it is 80 lines.

**New fixture, `--plan gallery_scroll`.** The same gallery page loaded with
`#innerscroll`: the panel holds a 2502px document — 40 numbered, individually
coloured rows, so a misplaced row is visible and no scroll detector can alias one
onto another — inside a 216px window, and the plan scrolls that window
0 → 1257 → 2286 → 457 while the page itself never moves after bringing the panel
into view. The recorder gained a `scrollEl` step for it, and the ground truth
carries the inner offset per frame.

**Result on it:** the document stays 1457px (only what the outer scroll covered,
which is the point — a document model must not grow because something inside it
scrolled), and the subframe is reported with one inner document of **300×2433px**
reconstructed from a 209px window, rows 1 through 40 in order. Ground truth for
that window is 2495px, so it is 62px short — 2.5% over 335 dead-reckoned steps,
which is what dead reckoning costs inside a region with less content to correct
against than a page has. Asserted as `innerDocCount` and `innerDocExtent`.

**Two measurements changed the design.** Both were wrong in the same direction —
counting the page's own movement as the subframe's:

- A region that SCROLLS is invisible to change detection built for swaps: it only
  changes the boundaries between its rows, so its changed mask is a set of thin
  strips, each below the pixel floor. Measured: 451 frames of a panel scrolling
  2286px produced **zero** detected regions. A morphological close over
  `SUBFRAME_CLOSE_PX` merges strips that belong to one region while leaving two
  regions of a page apart.
- An inner run has to be bounded by the DOCUMENT's own movement, because a
  rectangle fixed in frame coordinates looks at page content sliding past while
  the page scrolls, and the inner classifier cannot tell that from the subframe
  scrolling itself. Without that bound the fixture's inner document came out 476px
  too tall. Bounding it on the classifier's `Scroll` verdicts is not enough
  either — on the auto-refreshing news portal those verdicts are 4 for ~25 real
  scrolls, and using them yielded a 4085px "inner document" that was the article
  scrolling. The reliable signal is the per-frame OFFSET trace, so the recursion
  runs at the end of the session pass, after that trace exists.

**On real data.** The embedded prototype now yields two inner documents taller
than its own window: a `Meldeadresse` form (703px of a 507px window — street,
house number, postcode, city, country, with its BACK/NEXT bar) and a TAN
confirmation screen (715px). Both were previously three unrelated crops. rw1's
content column is reported as a subframe with a legible 1325px inner document —
the page refreshes its own teasers, so "a region that changed in place and then
scrolled" is a true description of it, and the stitch is genuinely better than
what the page-level pass got for that visit (1015px). Worth stating plainly: the
label is generous, the image is right.

**Cost:** one classification pass over the subframe's rectangle, plus the
stitcher's own passes for each run that scrolled — and only for runs that
scrolled, since a run that never moved is already shown whole by its single
settled frame. Measured end to end: the news portal 15.6s → 15.8s, the embedded
prototype 62s → 66s. All 19 sessions PASS with every previous `totalError` and
ghost score identical.

**Not done:** the inner document has no equivalent of the scrollbar map, so its
extent is dead reckoning only and it cannot say how much of the inner document it
never reached. Nested subframes (a subframe inside a subframe) are not attempted —
nothing in the corpus has one, so there is nothing to measure a design against.

### Output names a person can read, and a subframe belongs to a page — 2026-07-31

Asked for as a naming tidy-up, and it turned out to be a modelling gap. Subframes
were detected and reported at SESSION level, so a written file could not say which
page it came from, and the benchmark did not archive them at all — they stayed in
`outputs/` under the working name `pending_sub0_state00.png`, looking like they
belonged to whichever run came next.

Both are fixed by the same idea: **a subframe is on a page.**

- **Attribution.** Detection stays session-wide, because a region is one region
  whether or not the session navigated while it was there — but the states, inner
  documents and change count are now split by the page each frame belonged to,
  using the per-frame page trace. A region that outlives a navigation appears under
  each page it was on, with that page's share of its evidence. Measured on
  rw5_mobileframe: one phone mockup at 370×462, its 13 settled states distributed
  as 1 / 5 / 2 / 5 across pages 0, 1, 4 and 6.
- **Names.** Zero-padded to three digits and page-first, so alphabetical order is
  reading order:

  ```
  run_…_err148.png                              the page
  run_…_err148_page000_sub000_doc000.png        its subframe, stitched
  run_…_err148_page000_sub000_state000.png      that subframe's states
  run_…_err148_page000_sub000_state001.png
  ```

  Three digits because two is not enough for a real session (one recording
  produced eleven pages, and a page can hold several subframes with dozens of
  states). A page can hold `sub000`, `sub001`, … — an ajax gallery and a
  scrollable comment section below it are two.
- **Archiving.** The benchmark now archives every `pending*.png` the run wrote by
  substituting the prefix, rather than enumerating the two names it used to know
  about. The tool decides what it writes; the harness should not have to be told
  again each time.

Schema 5 moves `subframes[]` inside the page it belongs to. All 19 sessions PASS
with every `totalError` and ghost score unchanged — this moved names and nesting,
not pixels.

### A scroll that belongs to a region, not to the document — 2026-07-31

Spotted by eye in an output: the bottom third of one phone-flow page held a
repeated fragment of the host page — its "Made in InVision" badge four times, two
extra phone bezels — below the single phone screen. It looked like ghosting. It was
not.

**Measured.** That page recorded four scroll events (dy 54, 133, 45, 55 → vt 287),
so its canvas was 1307px for a 1020px viewport. On every one of those four
transitions the change was inside the phone mockup — 45-58% of the mockup repainted
— and the rest of the frame changed by **0.001-0.37%**. The page never moved; the
mockup's own content scrolled and the document was charged for it. Everything
visible follows: the canvas grew by 287px that never existed, and because the visit
had only those four "scroll" transitions — below the six the chrome accumulator
needs — nothing was ever confirmed as fixed, so the page's viewport-fixed furniture
was written once per phantom offset. The badge's canvas rows (973, 1159, 1205, 1260)
match the vt increments exactly.

**The fix, and why it needed two attempts.** A dy whose evidence is contained in a
region that changes in place belongs to that region. The rectangles are already
found — `SubframeTracker` locates them in pass 1 without needing the chrome
estimate — so pass 2 now receives them the same way it already receives the noise
mask, and the classifier refuses a proposal when ≥
`SUBFRAME_CONFINED_CHANGE_FRAC` of the changed pixels sit inside one. Measured on
the misattribution: 99.4% of the changed pixels inside the mockup, 887px of a
1.43M-pixel remainder moving. A real document scroll spreads its change across the
whole frame and cannot be contained in a bounded rect.

That much, alone, broke `session_manyjumps`: totalError 122 → 965 with 11 phantom
navigations. Its ten instant leaps replace the whole CONTENT COLUMN at once, and
the column then settles — so the column reads as a region changing in place, and
refusing dy inside it refused the fixture's genuine scrolls. There is no local way
to tell "this narrow column is the document" from "this narrow column is a subframe
inside a static document": both leave their surroundings unchanged, both pass the
scroll veto, and on these recordings there is no scrollbar to arbitrate (CDP
captures have none; the study recordings' viewer hides it).

What does separate them, measured on both populations, is **width**:

| | region width / frame width |
|---|---|
| embedded phone screens, a fixed video ad, the three lab panels | **19-25%** |
| a page's content column (manyjumps, and rw1's teaser slots) | **50-61%** |

A document's content area spans most of the viewport width — that is what makes it
the document — while a subframe is an element inside the content flow. So only a
region narrower than `SUBFRAME_MAX_WIDTH_FRAC` (0.40, in the empty middle of that
gap) may refuse a scroll. It is a heuristic, and it is labelled as one; the honest
part is that both sides of it are measured.

**Result.** All 19 sessions PASS with every fixture at its established
`totalError`. The seed sweep shows no new instability: the same two sessions wobble
as before (rw1 1 page for 7 of 8 seeds, rw5 5/6/7) and the other ten are identical
under every seed. Across the eight sessions of one phone-flow study, document that
the pages claimed but never had: **7255px → 1134px**, and the page whose output
started this is now nine pages of exactly one viewport each, with the repeated
furniture gone and one coherent screen on each.

The robustness goal (see the standing-goal section) gets its dedicated pass:
everything the corpus has surfaced that isn't fixed yet.
- **Mouse cursor**: small moving diff blobs erode the fixed bands and can
  trigger UNSTABLE — ignore small connected diff components in
  `accumulate_fixed` and in the transition classifier.
- **Smooth/animated scrolling**: sub-pixel and easing produce non-integer,
  frame-varying dy — verify histogram voting copes; consider ±0.5 px vote
  smearing; watch cumulative drift (frame-vs-own-canvas re-localization via
  the M4 matcher is the mitigation).
- ~~**Large jumps** (Page-Down, anchor links, `|dy| ≥ MAX_SCROLL`): currently
  rejected → would be misread as a page change; instead re-localize within the
  page using the M4 frame-vs-canvas matcher.~~ **Done 2026-07-27** — see
  *Intra-page re-localization* above (content evidence where the jump lands in
  seen territory, scrollbar evidence where it does not).
- Variable fps / dropped frames, stronger compression, DPI/zoom changes,
  browser UI at the top of the recording (crop detection).

**Exit criteria:** the full real-world corpus reconstructs with no crashes and
visually acceptable stitches; every corpus recording has an entry in the
results log; fixture benchmarks unregressed.

---

## Coverage cases + sensitivity (2026-07-04)

Four axis-isolating test sessions (user-specified) plus the production
`sensitivity` parameter:
- `session_noscroll` — no scroll at all → viewport-sized stitch, zero scroll
  events, **no fixed elements claimed** (unknowable without scrolling; the
  benchmark enforces "must not claim").
- `session_plain` — scrolling with **no fixed elements** (full-width plain
  document): scroll detection independent of chrome.
- `session_revisit_noscroll` — two pages + history back, **never scrolling**:
  page-change + REVISIT independent of chrome and scrolling.
- `session_cookie` — the borderline case: a cookie banner closes via DOM
  mutation on the same page. Under `-S normal`: one stitch (a slim frozen
  banner remnant — one of the two valid outcomes). Under `-S high`: two
  stitches (with banner / without).

These cases forced two classifier improvements (each measured):
- **PAGE_CHANGE = band SPREAD, not area fraction**: the fraction of 32px bands
  changed ≥5%. Mask-free (works with zero scrolling, where the online chrome
  estimate cannot learn) and cleanly separated: navigations 0.75–0.86 vs
  banner 0.14 vs animation/hover 0.00. Normal threshold 0.5, High 0.10.
- **Top-K scroll proposals**: periodic content (full-width striped
  placeholders) can put an alias above the true dy in the vote histogram; the
  veto now walks the top-3 proposals and takes the first that explains. The
  ±1px noise gate also became a proposal (veto-decided), recovering the six
  long-missed |dy|=1 events — session_page1 dyErrorSum 6 → **0**.

`-S normal|high` (CLI): Normal = new page only on real navigation; High = new
stitched screenshot per bigger mutation, and visits are never merged (each is
its own document version — the production semantics). All **eight** sessions
PASS `--all` under Normal.

## Sequencing rationale

M2 (typed transitions) is the foundation everything else consumes and makes
scroll-events-with-timestamps — a headline deliverable — nearly free. M3 needs
M2's segmentation signal. M4 needs M3's per-page stitchers to have something to
merge into. M5 (quality) benefits from M4 because revisit-merging is itself a
ghosting source. M6 freezes the external contract once the moving parts exist.
M7 is the dedicated hardening pass — but the real-world corpus grows from M3
onwards, so robustness pressure is applied throughout, not deferred.
