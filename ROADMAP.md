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

**Corpus arrived (2026-07-04):** five production sessions (webs elements, with
the DOM system's stitches as references) live in `test_data/realworld/` —
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

Side observation, deliberately NOT acted on: while a scrollbar is present a
genuine fixed right SIDEBAR can no longer be detected, because `right_w_` counts
contiguous fully-fixed columns inward from the edge and the bar's moving thumb
stops that scan at the first column. No session in the corpus has both, so there
is nothing to measure a fix against.

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
