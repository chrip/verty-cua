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
      ├── PageRegistry           page fingerprints, identity             (M4)
      │    └── PageIdentifier    frame ↔ known-page matching             (M4)
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

## Milestone 5 — Smart-stitch quality: anti-ghosting & dynamic content

**Goal:** the "smart" in smart screenshot — no duplicated footers, no smeared
animations, no scrollbar streaks, on the real-world corpus, not just fixtures.

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

## Milestone 6 — Session report: the full output contract

**Goal:** machine-readable output equivalent to the production algorithm's
contract — with **confidence fields** wherever the pixel version infers what
the DOM version could observe.

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

## Milestone 7 — Real-world hardening

The robustness goal (see the standing-goal section) gets its dedicated pass:
everything the corpus has surfaced that isn't fixed yet.
- **Mouse cursor**: small moving diff blobs erode the fixed bands and can
  trigger UNSTABLE — ignore small connected diff components in
  `accumulate_fixed` and in the transition classifier.
- **Smooth/animated scrolling**: sub-pixel and easing produce non-integer,
  frame-varying dy — verify histogram voting copes; consider ±0.5 px vote
  smearing; watch cumulative drift (frame-vs-own-canvas re-localization via
  the M4 matcher is the mitigation).
- **Large jumps** (Page-Down, anchor links, `|dy| ≥ MAX_SCROLL`): currently
  rejected → would be misread as a page change; instead re-localize within the
  page using the M4 frame-vs-canvas matcher.
- Variable fps / dropped frames, stronger compression, DPI/zoom changes,
  browser UI at the top of the recording (crop detection).

**Exit criteria:** the full real-world corpus reconstructs with no crashes and
visually acceptable stitches; every corpus recording has an entry in the
results log; fixture benchmarks unregressed.

---

## Sequencing rationale

M2 (typed transitions) is the foundation everything else consumes and makes
scroll-events-with-timestamps — a headline deliverable — nearly free. M3 needs
M2's segmentation signal. M4 needs M3's per-page stitchers to have something to
merge into. M5 (quality) benefits from M4 because revisit-merging is itself a
ghosting source. M6 freezes the external contract once the moving parts exist.
M7 is the dedicated hardening pass — but the real-world corpus grows from M3
onwards, so robustness pressure is applied throughout, not deferred.
