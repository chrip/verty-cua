#pragma once
#include <opencv2/opencv.hpp>
#include <cstdlib>

// ── Constants ─────────────────────────────────────────────────────────────────
constexpr int BLOCK_SIZE = 16;          // pixels per block side
// Max per-frame scroll searched. Real recordings contain Page-Down/anchor
// jumps and fast flings far beyond smooth-scroll steps. 880 keeps a thin
// overlap band on a ~900px viewport: the few blocks that land there vote the
// true dy, the top-K proposals surface it, and the explain-veto (measured on
// the overlap only) confirms it. Jumps at/beyond the viewport height share no
// pixels between frames — genuinely unrecoverable by local matching; they
// read as page changes and the registry may re-localize them later.
constexpr int MAX_SCROLL = 880;
constexpr int SCROLL_STEP = 1;          // pixel step for scroll search
constexpr int SCROLL_SAMPLES = 120;     // blocks sampled for scroll histogram
constexpr int SAD_THRESHOLD = 18;       // max mean absolute diff to call a match
constexpr int FIXED_CONFIRM_FRAMES = 6; // stable scroll events before a pixel is FIXED
constexpr double IDENTICAL_FRAME_MAD = 2.0;      // below: duplicate frame / pause
// Smallest share of the frame the chrome+noise estimate may leave before it is
// treated as unusable for normalising that test (a nearly-everything estimate
// would turn any flicker into 100% of the active area).
constexpr double IDENTICAL_MIN_ACTIVE_FRAC = 0.05;
// A page change is a change SPREAD over the viewport: the fraction of 32px
// horizontal bands whose changed-pixel share exceeds BAND_CHANGED_MIN_FRAC.
// Spread needs no chrome mask (works on no-scroll sessions where the online
// chrome estimate never learns) and separates cleanly — measured: navigations
// 0.75–0.86, cookie-banner mutation 0.14, animation/hover 0.00.
constexpr double PAGE_CHANGE_MIN_SPREAD = 0.5;   // Sensitivity::Normal
constexpr double BAND_CHANGED_MIN_FRAC  = 0.05;

// ── Sensitivity ───────────────────────────────────────────────────────────────
// Mirrors the production DOM-based parameter:
//   Normal — a page change only on a real navigation (large unexplained change).
//   High   — a new stitched screenshot for every bigger DOM mutation (e.g. a
//            cookie banner closing): a much lower changed-area threshold, and
//            visits are NEVER merged (no REVISIT) — each one is its own
//            document version. Caveat of the pixel world: heavy animation is
//            indistinguishable from mutation and will also split.
enum class Sensitivity { Normal, High };
constexpr double PAGE_CHANGE_HIGH_SPREAD = 0.10;   // banner-close = 0.14 ⇒ splits
// A detected scroll dy must also EXPLAIN the transition. After shifting the
// previous frame by dy, the overlap is cut into horizontal bands and each
// band's changed fraction is measured; the MEDIAN band must be nearly clean.
// The median is robust against a bounded animated region (which only pollutes
// its own bands) while a false dy on a page change leaves most bands dirty.
// Measured: true scrolls (animation raging in view) median 0.000; false
// scrolls 0.081 and 0.909.
constexpr double SCROLL_RESIDUAL_MEDIAN_MAX = 0.05;
constexpr int    RESIDUAL_BAND_H = 32;   // band height (px) for the median
// The residual can only be measured where something changed, so bands that did
// not change are excluded from it — but for a nonzero dy they are not neutral.
// Content moves through EVERY band, so a textured band that stayed put
// contradicts the hypothesis, and a dy carried by a handful of changed bands is
// carried by coincidence: two flat regions of the same shade agreeing across the
// shift. Measured: an in-place panel swap 320px below a same-shade page header
// produced a perfect median residual (0.000) at dy = −320 from 3 changed bands
// out of 18 — on every one of ten repetitions.
// So a minimum share of the informative bands must have changed. Measured over
// 1232 true lab scrolls: worst 0.393 (|dy| ≤ 4px), median 0.64-0.87; over 213
// true scrolls in the real corpus: worst 0.545, median 0.82-1.00. The false
// panel swap sits at 0.167, so 0.30 has margin on both sides.
// It also declines a genuine scroll INSIDE a subframe (an inner scrollable div),
// which changes only its own bands. That is correct: an inner scroll is not a
// document scroll, and counting it as one corrupts every offset after it.
constexpr double SCROLL_MIN_CHANGED_BAND_FRAC = 0.30;

// The stitched interior is cropped this many rows inside the detected
// header/footer bands, for two reasons: (a) a band under-detected by a pixel
// or two would smear a chrome line at every scroll step; (b) chrome drop
// shadows (e.g. a 6px header box-shadow) darken the adjacent content rows —
// pixels that are neither stable (they blend with scrolling content) nor
// clean content, and would poison every write's edge rows. The margin must
// clear the shadow. The header/footer stamps are extended by the same margin
// (sourced from the shallowest/deepest frame, where those rows show the
// correct adjacent content), so no gap remains.
constexpr int INTERIOR_ROW_MARGIN = 12;

// ── Per-row modal reconciliation ─────────────────────────────────────────────
// A document row is normally observed by many frames, and they do not always
// agree. Keeping the FIRST observation and freezing it (the rule until
// 2026-07-28) trusts the least trustworthy sighting: a page is still SETTLING
// when a visit begins — cookie banners still up, late-loading content still
// shifting the layout — so the first sighting is the one most likely to be
// transient. Measured: session_cookie baked the dismissed banner across a text
// line and an image (ghost madMean 6.14, badBlocks 4.56%), and rw3's page 2
// lost 12 of its first headline's 31 rows to a 12px reflow that happened while
// the opening frames were captured.
//
// So every observation VOTES and the appearance most frames agree on wins.
// Votes are compact row SIGNATURES (per-bucket column means) rather than full
// rows, so tracking several candidate appearances per row costs bytes instead
// of a canvas copy each: 3 slots × 16 buckets × 3 channels ≈ 2MB for an
// 11618-row page.
// Bins per row signature, and the difference below which two observations are the
// SAME appearance. These decide the vote's IDENTITY test, and 16 bins with a
// tolerance of 12.0 were too coarse to be one: a row of mostly-white page with
// sparse dark text has nearly the same bucket means whatever the text says, so
// visually different appearances merged into one candidate and the elected
// exemplar became whichever frame arrived last in the merged group — a different
// frame for each row, which renders as a double exposure. Measured on an embedded
// prototype tapped through thirteen screens: the canvas showed several of them
// superimposed, and the same defect was already inside session_gallery, whose
// ghost score carried a worst block of 83 MAD.
//
// At 64 bins with a tolerance of 3.0 the appearances separate. Measured:
//
//        fixture                16 bins / 12.0            64 bins / 3.0
//        session_gallery ghost  3.07 / worst 83.06 / 0.98%  2.58 / 5.66 / 0.00%
//        session_cookie ghost   2.57 / 4.32 / 0.00%         unchanged
//        session_hostile ghost  2.77 / 13.35 / 0.00%        unchanged
//        session_page1 error    106px                       unchanged
//
// The slot count is NOT the lever, which is worth recording because it looks like
// it should be: raising ROW_VOTE_SLOTS from 3 to 24 changed the same output by
// 2.1 MAD, i.e. nothing. Boyer-Moore eviction was never what lost the majority;
// the signature could not tell the candidates apart in the first place.
constexpr int    ROW_SIG_BINS   = 64;
constexpr int    ROW_VOTE_SLOTS = 3;
constexpr double ROW_SIG_MATCH  = 3.0;

// ── Interior fixed overlays (floating buttons, cookie banners) ───────────────
// A viewport-fixed element that is NOT edge-anchored appears in the pixel
// fixed mask as an interior blob. Genuine overlays are compact and textured;
// stable whitespace (the historical false-positive that ruled out floating
// rects) is sprawling and uniform, so it fails these filters.
constexpr int    OVERLAY_MIN_AREA   = 576;   // ≥ 24×24 px of confirmed-fixed pixels
constexpr double OVERLAY_MIN_FILL   = 0.5;   // fixed-pixel fill ratio of the bbox
constexpr double OVERLAY_MIN_STDDEV = 8.0;   // texture: uniform blobs are not UI
constexpr int    OVERLAY_PAD        = 4;     // bbox padding (anti-aliasing, shadow)

// Reported uncertainty of the fixed-band extents (session report). The bands
// can only OVER-detect — flat content adjacent to the chrome reads as stable
// and is absorbed into the band (a documented non-decision: the margin is
// provably invisible in the output). Largest over-detection measured on the
// fixtures: +39px (sidebars). The true band edge lies within
// [detected − uncertainty, detected].
constexpr int FIXED_BAND_UNCERTAINTY_PX = 48;
constexpr int PAGE_STABLE_CONFIRM = 3;  // stable transitions that close a page-change window
// A visit whose first frame is nearly uniform (gray stddev below this) is a
// blank loading screen, not a page — it produces no stitch and no navigation.
constexpr double BLANK_VISIT_STDDEV = 6.0;
// Shortest run of never-observed document rows worth reporting as a gap.
// Single rows at a write boundary are offset quantization, not skipped content.
constexpr int GAP_MIN_ROWS = 8;

// ── Persistent dynamic noise (fixed-position video ads, tickers) ─────────────
// A pixel that changes during MANY non-scroll transitions is persistent
// viewport-anchored noise. The confirm threshold is deliberately HIGH: a
// scrolling animated element only accumulates for the length of one reading
// pause at a given viewport position (≤ ~18 frames on the fixtures) before it
// moves on — the M5 lesson that killed the first dynamic-mask attempt — while
// a fixed video ad accumulates for the whole session. Confirmed-noise pixels
// are excluded from the page-change spread and from the scroll veto's
// evidence; they are NEVER used for detector sampling exclusion.
constexpr int DYNAMIC_CONFIRM_FRAMES = 25;
// ...but a COUNT alone conflates "changes often" with "the session is long". A
// 2000-frame session in which the user taps through 40 screens crosses 25 just
// as surely as a 124-frame session with an ad churning every frame, so the count
// swallowed exactly the regions this project most needs to see: a gallery, a tab
// switcher, a phone screen inside a subframe — punctuated in-place change, which
// has the same per-pixel signature as an ad and none of its persistence.
//
// So a pixel must also change in a large enough SHARE of the non-scroll
// transitions it was observed in. Measured per-pixel change rate inside the
// region the count alone confirmed as noise: rw1's genuine fixed video ad
// 23.6% (p90 26.8%), against 3.9% / 1.6% / 1.9% for three phone-flow sessions
// (p90 at most 5.2%). 10% sits in the empty middle of that 6-15x gap.
//
// The rate also serves the original intent BETTER than the count did: a
// scrolling animation dwells at one viewport position for a single reading pause
// of <=18 frames, which is 4.5% of a 400-frame session — correctly not noise,
// and it stays correct however long the session runs.
constexpr double DYNAMIC_MIN_CHANGE_RATE = 0.10;
// Ignore the noise mask entirely if it claims more than this fraction of the
// frame (safety against runaway accumulation).
constexpr double DYNAMIC_MASK_MAX_FRACTION = 0.4;

// ── Subframes: a bounded region that changes while the document does not ────
// An embedded prototype, a gallery, a tab switcher, a playing video ad. See
// subframe_tracker.hpp for why the only usable separator is whether the region
// ever HOLDS STILL, and for the measurements behind these numbers.
//
// Minimum changed pixels for a transition to count as a change of a region at
// all. Measured over the corpus: a moving cursor, a blinking caret or a hover
// highlight change 100-5000 px, while the smallest genuine screen swap in the
// real recordings changed 10112. Duplicate frames change exactly 0 (video
// encoders emit identical frames during a pause, on every real recording
// measured).
constexpr int    SUBFRAME_MIN_CHANGED_PX = 5000;
// A subframe is BOUNDED. A real navigation repaints the whole viewport; the
// corpus's largest genuine subframe is a fixed video ad at 31% of the frame,
// and an embedded phone screen is 12%.
constexpr double SUBFRAME_MAX_AREA_FRAC  = 0.60;
// Transitions a region must hold perfectly still for, after changing, for that
// appearance to count as a STATE somebody could have looked at. Measured share of
// in-place changes that reach it: lab animated panel 0%, corpus video ad 0%, real
// embedded phone screens 31% and 39%, lab panel tapped 30 times 100%.
//
// It was 4, which cost real screens on a recording whose prototype animates a zoom
// between screens and switches them fast: at that recording's own 4.80 fps, four
// still transitions mean 0.83s of stillness, and many of its screens were held for
// less. Swept end to end, screens found against what the negative cases keep:
//
//        settle   embedded flow   lab 30 taps   animated panel   video ad
//             4              16            30                0          0
//             3              22            30                0          0
//             2              26            30                0          0
//             1              28            30                0          0
//
// The negative cases cost nothing at any setting, and the reason is structural: a
// playing video changes on EVERY transition, so it never has a still frame at all,
// while a screen a human looked at always does. 2 rather than 1 because the corpus
// only contains animations that repaint every frame — one repainting every OTHER
// frame would satisfy a single still transition and produce a state per frame pair,
// and there is no recording here to vouch otherwise. Two consecutive still
// transitions excludes that too.
constexpr int    SUBFRAME_SETTLE_FRAMES  = 2;
// Settled states that make a region a SCREEN FLOW rather than noise. Two is not
// enough: the lab hostile fixture's animated region happens to hold still twice in
// its 204 frames and would qualify on two. Three is the smallest number that
// separates it from the flows measured (lab panel 30, embedded phone screens 19
// and 21) — and a sequence of screens worth keeping has at least three.
constexpr int    SUBFRAME_MIN_STATES     = 3;
// In-place changes below which a region is a one-off, not a subframe — a cookie
// banner closing changes once and must not be reported as a sequence of screens.
constexpr int    SUBFRAME_MIN_CHANGES    = 3;
// Overlap ratio (intersection over union) at which a change belongs to a subframe
// already seen. Grouping by "do the boxes touch" instead chains adjacent changes
// until one group covers the viewport — measured, one grew to 1800×956 of a
// 1918×956 frame. A ratio keeps a gallery and a ticker apart while tolerating the
// jitter of which part of a region repainted.
constexpr double SUBFRAME_MIN_IOU        = 0.35;
// Gap (px) across which two changed patches are treated as one region. A region
// that scrolls its own content changes only the boundaries between its rows, so
// without closing that gap its changed mask is a set of thin strips, each below
// SUBFRAME_MIN_CHANGED_PX, and the region is never detected. 25px is wider than
// the row gaps measured in the fixtures (6-14px) and far narrower than the space
// between two regions of a page.
constexpr int    SUBFRAME_CLOSE_PX       = 25;
// Inner scroll transitions a run must contain before it is worth stitching. A run
// that never scrolled is already fully shown by its one settled frame, and
// stitching it would cost three more passes over the video to reproduce a crop
// that already exists.
constexpr int    SUBFRAME_MIN_INNER_SCROLLS = 2;
// Share of a transition's changed pixels that must lie inside one subframe for the
// change to be that subframe's rather than the document's. Measured on the
// misattribution this exists to stop: 99.4% of the changed pixels were inside the
// phone mockup (143k of 144k), with 887px of a 1.43M-pixel remainder moving. A
// real document scroll spreads its changed pixels across the whole frame, so it
// cannot be contained in a bounded rect — the margin either side is wide.
constexpr double SUBFRAME_CONFINED_CHANGE_FRAC = 0.90;
// ...but only a region NARROWER than this share of the frame may refuse a scroll.
// A document's content area spans most of the viewport width — that is what makes
// it the document — while a subframe is an element inside the content flow and is
// narrower. Without the gate the rule misfires on exactly the case it looks like:
// a leap across a page replaces the whole content column at once, so the column
// reads as a region changing in place and settling, and refusing dy there cost
// session_manyjumps its re-localization (totalError 122 → 965, 11 phantom
// navigations). Measured width shares: subframes that must refuse 19-25% (embedded
// phone screens, a fixed video ad, the lab panels), content columns that must not
// 50-61%. The gate sits in the empty middle.
constexpr double SUBFRAME_MAX_WIDTH_FRAC = 0.40;
// Of the FRAME's width, and measured to be the better denominator than the one that
// sounds more principled. A region's size relative to the document's own content
// area — the frame minus the chrome — is what "a frame WITHIN the content" ought to
// mean, and how much furniture surrounds that content is not the region's business.
// Tried, over all 144 candidates the corpus produces (16 that must be kept, 128 that
// must be refused), with the interior read the way PageStitcher reads it:
//
//              must keep      must refuse    separable by one constant?
//   width, frame     <= 38.0%     >= 49.6%   yes — 0.40 sits in the gap
//   width, interior  <= 62.1%     >= 49.9%   NO  — the ranges cross
//   area,  frame     >=  5.44%    <=  4.99%  yes — 0.05 sits in the gap
//   area,  interior  >=  6.26%    <=  6.49%  NO  — the ranges cross
//
// The reason is that the chrome fraction itself varies from 0% to 50% across the
// corpus, so dividing by the interior injects that variance into the very quantity
// being thresholded: it inflates the lab panels (24.7% -> 40.4% of width, on a
// 782px interior in a 1280px frame) while barely moving a real site's content column
// (49.6% -> 49.9%, on a 1874px interior in a 1918px frame), and the two populations
// meet. Keeping the hostile fixture's 486px panel would then admit a news portal's
// 951px article column and a landing page's 950px band.
//
// So this is frame-relative on purpose, and it is the frame that is wrong when the
// document does NOT fill the frame — a browser window inside a desktop recording,
// where a 316x255 panel is 7.0% of the browser's content area but 3.9% of a full-HD
// frame and is dropped. That case needs a viewport rectangle distinct from the frame
// (also needed by the scrollbar tracker, captured_extent, the scroll veto's band
// logic and the identical-frame active area — all of which read frame_w_/frame_h_ as
// "the viewport"), not a different denominator here. No corpus recording is windowed,
// so there is nothing to measure such a model against yet.
// A subframe's rectangle is a VIEWPORT rectangle, so reporting one asserts that the
// rectangle names the same content every time it is cropped. That holds only while
// the document does not move underneath it. If it does, consecutive states are
// crops of whatever happened to be at those coordinates — which is exactly what a
// product grid produced: hover previews on tiles that had scrolled between states,
// written as strips of page at inconsistent offsets.
//
// Measured document-offset spread across a region's own states: lab panels 0px,
// real embedded phone mockups 0px and 68px (the 68 being a residual false scroll of
// its own), against 120-720px for a scrolling product grid and 2759-3779px for a

// ── Anchor: does an in-place region move with the document? ───────────────────
// Scored per scroll transition, twice: the region's pixels in the current frame
// against the previous frame at the SHIFTED rectangle (it moved with the
// document) and at the SAME rectangle (it stayed put in the viewport). See
// SessionProcessor::measure_slice_anchors for why the alternatives — the overlay
// list, the noise mask, the offset spread — cannot answer it.
//
// Measured over every region the corpus reports — median MAD of each hypothesis
// over the transitions it could be scored on:
//
//   region                                    pairs   moved   stayed   verdict
//   rw1 sticky video ad, 481x956 at the edge     20  69.672    0.003   viewport
//   rw1 in-content ad, 302x331                   20   0.508   54.877   document
//   rw6 carousel, 501x241 (noise)                12   0.004   73.868   document
//   rw6 grid hover preview, 669x229 (3 pages) 11-15 0.004-0.274 26-44  document
//   lab panel tapped 30 times                   176   0.404   25.497   document
//   lab panel, its own inner scroll              46   0.414   30.671   document
//   lab panel, ANIMATED (never holds still)      46   0.422   24.327   document
//   lab hostile, two regions                    123 0.695/0.754 22/27  document
//   rw5 mockup (4 pages), rw7 flow                0       —        —   no verdict
//
// The chosen hypothesis is never worse than 0.754 and the rejected one never
// better than 22.24 — a factor of 29 with nothing in between, so the ratio bar is
// set 7x away from the worst observation. The three controls that matter are the
// regions that NEVER hold still (the animated panel, the inner-scrolling panel,
// the video ad): a region repainting itself pays the same penalty under both
// hypotheses, so all three are still decided correctly, which is the whole reason
// the comparison is made per transition instead of between distant frames. Asked
// between distant frames it does NOT separate — a product grid is periodic enough
// that two frames 600px apart look alike (28.9 staying vs 37.5 moving for a
// region confirmed in flow at 170:1 here).
//
// The two pages that score nothing are pages that never scrolled: with no scroll
// transition there is no evidence, and no verdict is the honest answer. Both bars
// are floors on evidence, not on strength: 0.929 is the worst vote fraction
// observed (13 of 14, a grid whose repeats occasionally favour the wrong shift).
// Cost: one extra sequential read where a page scrolled at all — rw6 177.0s ->
// 178.3s, session_gallery 26.6s -> 27.6s, rw7 unchanged (skipped).
constexpr double ANCHOR_MIN_VOTE_FRAC   = 0.75;
constexpr double ANCHOR_MAX_MAD_RATIO   = 0.25;
constexpr int    ANCHOR_MIN_PAIRS       = 3;
// The shifted rectangle has to stay in frame, so a transition only scores the
// rows both hypotheses can see. Below this many rows the comparison is a sliver
// of a region and says nothing about it.
constexpr int    ANCHOR_MIN_OVERLAP_PX  = 40;


// news article's content column.
constexpr long   SUBFRAME_MAX_ANCHOR_DRIFT = 100;
// Smallest share of the frame a subframe may be. Without it, every text block of
// a page that reloads its own content qualifies — measured on the auto-refreshing
// news portal: 79 candidate regions of 0.3-3% of the frame, several with two
// settled states each. The regions this is for are an order larger: the lab panel
// is 7% of its frame, an embedded phone screen 12%, a fixed video ad 18%.
constexpr double SUBFRAME_MIN_AREA_FRAC  = 0.05;
// How much of a box must lie INSIDE a region to belong to it. A screen swap
// repaints all of its region on one tap and a corner of it on the next, so its
// changes have wildly different shapes: attribution has to be containment, not
// overlap ratio (measured: by ratio, an embedded phone screen kept 2 of its 29
// settled states).
//
// 0.9 rather than 0.7, because a state is an appearance OF the region and what
// changed therefore has to be in it, not merely overlapping it. Measured on a
// product grid: a hover on the tile at x=1130 lay 77% inside the region starting at
// x=1213, so it was attributed there and written as a state — giving two crops of
// the same two tiles that differ only by a pale overlay on a tile mostly outside
// the frame. Every state of the embedded phone screens and of the lab panels lies
// fully inside its region, so the higher bar costs them nothing.
constexpr double SUBFRAME_CONTAINED_FRAC = 0.90;
// Two states of one region must actually look different. Measured share of pixels
// that differ between consecutive states, over the whole corpus: two pairs at 0.00%
// and 0.07% — the same appearance recorded twice, because something elsewhere in the
// region settled again — and then nothing until 5.6%, with real screen changes at
// 7.7% and up. 1% sits in that gap with room on both sides.
constexpr double SUBFRAME_STATE_MIN_DIFF_FRAC = 0.01;

// ── Vertical scrollbar: absolute-offset evidence for unexplained jumps ───────
// A user DRAGGING the scrollbar thumb (or flinging) moves the document by
// thousands of pixels between two frames — measured on rw4_portal: 4356px in
// one 3.89fps frame step. Past `viewport − header − footer` two consecutive
// frames share NO pixels at all, so neither frame-to-frame matching nor
// frame-to-canvas matching has anything to work with; measured on that very
// jump: "[registry] visit [151..158] vs page 1: confidence 0 conc 0 blocks 0".
// The browser's own scrollbar is then the ONLY pixel signal left that reveals
// where in the document the viewport sits. It is read geometrically: the bar is
// a column band carrying exactly TWO gray levels (a light track and a darker
// thumb); the thumb's TOP encodes the scroll offset and its HEIGHT encodes the
// document height (thumb_h / track_h = viewport_h / docHeight).
constexpr int    SB_SEARCH_COLS    = 64;   // columns searched inward from the right edge
constexpr int    SB_LEVEL_TOL      = 6;    // gray tolerance when binning one level (JPEG noise)
constexpr int    SB_MIN_CONTRAST   = 15;   // track↔thumb gap; measured 47 idle, 73 hover, 121 dragged
constexpr double SB_LEVEL_COVER    = 0.90; // the two levels must cover this share of the column —
                                           // the arrow buttons are the remainder (2×17 of 930 = 3.7%)
constexpr double SB_RUN_PURITY     = 0.90; // share of the darker level lying inside ONE run
constexpr int    SB_MIN_THUMB_H    = 10;   // shorter runs are glyphs/noise, not a thumb.
                                           // The arrow-button glyphs are 3–4 rows in every real
                                           // session; the smallest real thumb measured is 65px.
constexpr int    SB_MIN_WIDTH      = 6;    // narrowest bar accepted; measured 16px on the corpus
constexpr int    SB_MAX_WIDTH      = 24;   // widest accepted: a broad "band" is page content that
                                           // spans the viewport width, not a scrollbar
constexpr int    SB_EDGE_SLACK     = 3;    // non-bar columns tolerated at the very right edge
constexpr double SB_BAR_FRAME_FRAC = 0.6;  // share of observed frames a column must look bar-like in
// The thumb MUST MOVE. Any static dark block in the right-edge columns is
// otherwise a flawless two-level column: measured on the lab fixtures, whose
// 72px dark fixed header reaches x=1258..1279 and was accepted as a 14px-wide
// "scrollbar" with zero travel. Real thumbs travel the track (measured spans:
// 355px on rw4's deep page, 696px on rw2, 773px on rw3).
constexpr int    SB_MIN_THUMB_TRAVEL = 8;

// Calibration of thumb_top → document offset. The slope is fixed by GEOMETRY:
// a = viewport_h / thumb_h (one thumb pixel is worth one viewport height of
// document per thumb height). Measured on rw4 — deep page geometric 12.917 vs
// least-squares 12.966 (0.4% apart), short page 1.6146 vs 1.6161. The
// least-squares fit is preferred where the anchors span enough track, because
// it absorbs the thumb's sub-pixel rendering (max residual 7.0px over vt
// 0..4612 from 83 anchors, vs 14.9px for the geometric slope); the geometric
// slope is both the fallback for a narrow anchor span AND the sanity gate that
// a mis-detected column cannot pass.
constexpr int    SB_MIN_CAL_POINTS  = 4;
// Thumb travel the anchors must span before the slope is FITTED instead of
// taken from geometry. A short baseline makes least squares worse than the
// closed form, not better — measured on rw4's deep page by extrapolating to a
// point 5300px beyond the calibrated range: 50px of travel errs +138px, 74px
// errs +25px, 213px errs +10px, 355px errs +10px, while the geometric slope
// alone errs −7..−34px regardless of travel. 200px is where the fit starts
// beating the closed form.
constexpr int    SB_MIN_CAL_SPREAD  = 200;
constexpr double SB_SLOPE_TOL_FRAC  = 0.15; // fitted vs geometric slope; measured deviation ≤ 0.4%
constexpr double SB_MAX_RESIDUAL_PX = 24.0; // measured max 7.0px (deep page), 1.4px (short page)
// The slope itself is only known to about this fraction (measured
// geometric-vs-fitted disagreement: 0.4% on rw4's deep page, 0.1% on its short
// one), so extrapolating far past the calibrated range adds proportional error.
// Declared, not corrected — an honest interval beats a false point estimate.
constexpr double SB_SLOPE_ERR_FRAC  = 0.005;
// Share of anchors the map may refuse before the map itself is refused. A map
// describing the real bar of a real document explains nearly every anchor
// (measured: 0 of 83, 0 of 55 and 0 of 329 rejected on rw4's two pages); a map
// fitted to something that is not a scrollbar cannot explain its own inputs
// (measured on the lab fling fixture's false-positive column: 192 of 214
// rejected). Defence in depth behind the tracker's movement test.
constexpr double SB_MAX_REJECT_FRAC = 0.25;
// The calibrated map also yields the DOCUMENT HEIGHT, which no other signal in
// this project can produce: vt = 0 puts the thumb at the track top, so
// track_top = −b/a, and the arrow buttons at the two ends are near enough the
// same size that track_h = frame_h − 2·track_top. The deepest reachable offset
// is then a·(track_top + track_h − thumb_h) + b. Measured on rw4: track_top
// 16.6 against a true 17, track_h 896.7 against 897, document height 11623
// against the reference system's 11624. This matters because a session that
// stopped early cannot know how much it missed from its own frames alone.
//
// The arrows are not exactly equal (measured 17px top, 16px bottom), so the
// symmetry assumption costs one thumb pixel of track height, worth a·1 document
// pixels. Two is the honest allowance.
constexpr int    SB_TRACK_SYMMETRY_PX = 2;
// Share of a page's scrollbar anchors that must agree on the thumb height before a
// DOCUMENT HEIGHT may be derived from them. The thumb height is the fingerprint of
// which document the map describes, so anchors that disagree describe different
// documents and the height belongs to neither. Measured on a shop catalogue whose
// page merged visits with thumb heights 279 and 362: the height derived from the
// dominant two thirds came out 67px short of what the frames had demonstrably
// covered, declared to +/-8px.
constexpr double SB_MIN_FINGERPRINT_SHARE = 0.80;
constexpr int    SB_THUMB_H_TOL     = 2;    // document fingerprint match; measured 72 vs 576 (≫ tol)
// A scrollbar-derived continuation is accepted only where content matching is
// blind BY CONSTRUCTION: the predicted offset must leave the page's canvas less
// overlap than the identifier needs to reach a verdict. Its verification grid
// steps 48px and demands ≥12 textured blocks, which one block row of a
// full-width interior already supplies — so ~64px of overlap is where content
// evidence begins. Above that the identifier HAD evidence and said "no match",
// and that verdict wins: contradictory evidence means the page changed, not
// that the viewport jumped.
constexpr long   SB_CONTINUATION_MAX_OVERLAP = 64;

// ── Page identity (REVISIT recognition) ──────────────────────────────────────
// Blocks sampled per candidate match. This is a SAMPLE SIZE, and the two numbers
// it feeds — concentration and confidence — are proportions, so their noise falls
// only as 1/sqrt(samples). At 120 that noise was the deciding factor in four of
// the corpus's twelve identity-bearing sessions: sweeping the RNG seed at a fixed
// 120 samples produced 1 OR 2 pages for rw1, 5 OR 6 OR 7 for rw5, and 1 OR 2 for
// both re-localization fixtures — i.e. the shipped verdicts were partly a
// property of seed 42, not of the recordings. Measured spread of the deciding
// vote proportion, 8 seeds:
//
//        decision                       120 samples      480 samples
//        rw1 reload-revisit             0.543 ± 0.081    0.514 ± 0.030
//        fling leap (must match)        0.520 ± 0.028    0.538 ± 0.010
//        sibling template (must not)    0.409 ± 0.055    0.416 ± 0.027
//
// 480 makes ten of the twelve sessions produce the same structure under every
// seed tried (from eight), and costs 5% of the deepest session's runtime
// (rw4 20.2s → 21.2s) because patch_mad made the vote loop cheap. The observed
// spread tracks the binomial prediction sqrt(p(1-p)/votes) closely, so this is
// sample noise and nothing subtler.
constexpr int    IDENT_SAMPLES = 480;           // blocks sampled per candidate match
constexpr double IDENT_MIN_STDDEV = 4.0;        // skip near-uniform blocks (match anywhere)
constexpr double REVISIT_MIN_CONFIDENCE = 0.5;  // matched-block fraction to accept a REVISIT
constexpr int    IDENT_VT_SLACK = 2;            // vote bin slack (px) around the peak
// The block vote proposes the offset with the loose SAD_THRESHOLD; the region
// VERIFICATION then counts matches with a tighter bar. Measured (revisit_noscroll,
// 2026-07-04): a true revisit renders identical pixels — 100% of interior blocks
// match at MAD<9 (compression floor) — while two DIFFERENT pages sharing the same
// chrome collide at only ~15% at MAD<9 but ~41% at the loose 18. The gap is what
// separates "same page, content changed" from "different page, same template".
constexpr double IDENT_MATCH_MAD = 9.0;
// A revisit is accepted when the overlap fraction clears REVISIT_MIN_CONFIDENCE
// OR a large absolute cluster of near-identical blocks survives at one offset
// (a heavily-reloaded page whose surviving fraction dips below the bar) — but
// in BOTH cases the vote concentration must clear REVISIT_MIN_CONCENTRATION,
// which is what separates a same-page reload from a sibling page sharing one
// chrome template.
//
// The threshold was 0.5, taken in 2026-07-05 from the one case that needed it:
// rw1's reload measured 0.526 and the bar went just under it. Measuring the whole
// corpus at 480 samples over 8 seeds shows what that costs — the threshold sits
// half a standard deviation from the case it was fitted to:
//
//        must MATCH (lowest)            rw1 reload             0.514 ± 0.030
//        must NOT   (highest)           sibling template       0.416 ± 0.027
//
// so 0.5 leaves +0.5σ on the merge that must happen and −3.1σ on the merge that
// must not. Centring it in the measured gap balances them at 1.6σ / 1.8σ. That is
// the best this statistic can do here: the gap itself is only 0.098 wide, and
// peak-vs-runner-up dominance — the obvious sharper alternative — was measured
// and separates those two cases WORSE (3.39 ± 0.74 vs 3.29 ± 0.25, overlapping),
// so it is not the answer. Anything better needs evidence the vote histogram does
// not carry.
constexpr double REVISIT_MIN_CONCENTRATION = 0.465;
constexpr int    REVISIT_MIN_MATCH_BLOCKS  = 40;
// A content match against the page the session was ALREADY on is not a revisit
// — the session never left. It is a scroll jump too large for frame-to-frame
// matching, re-localized against that page's own canvas. The two are separated
// by how much of the frame is PIXEL-IDENTICAL to what the canvas already holds:
// a jump re-shows literally the same pixels (measured on session_fling:
// confidence 1.000 and 0.913), while a page that RELOADED repaints most of the
// viewport (measured on rw1's auto-refreshing news portal: 0.317). Below the
// bar the visit stays a REVISIT — a reload IS a new document version.
constexpr double CONTINUATION_MIN_CONFIDENCE = 0.75;

// One page visit: a contiguous, stable frame range showing a single page.
// Frames between visits (navigation transitions) belong to no visit.
struct PageVisit {
    long firstFrame = 0;   // inclusive
    long lastFrame  = 0;   // inclusive
};

// ── Scroll detection result ───────────────────────────────────────────────────
struct ScrollResult {
    int  dy    = 0;      // dominant scroll (pixels, positive = scrolled down)
    int  votes = 0;      // histogram bin count at dy
    bool valid = false;  // enough evidence found
    // Top vote peaks in order (dy first). Periodic content (stripes, text
    // lines) can put an alias above the true dy — the classifier's veto walks
    // the proposals and takes the first one that explains the transition.
    std::vector<int> proposals;
};
constexpr int SCROLL_PROPOSALS = 3;

// ── Inline helpers ────────────────────────────────────────────────────────────

// Mean absolute difference between two same-size CV_8UC1 patches.
//
// This is the hottest code in the project: the scroll detector calls it up to
// 2·MAX_SCROLL times per sampled block per transition, and the page identifier
// once per candidate row of a whole page canvas. It is written as a raw loop
// rather than cv::absdiff + cv::mean because at the 16×16 sizes involved the
// OpenCV overhead — a temporary allocation and two kernel dispatches per call —
// dwarfs the arithmetic entirely. Measured on the identifier's column scan over
// an 11618-row canvas: 322ms for the OpenCV form against 12ms for this one, 27×
// for the SAME result. Bit-identical, not merely close: both sum exactly (a
// 16×16 patch sums to at most 65280) and the quotient k/256 is exactly
// representable in float.
inline float patch_mad(const cv::Mat& a, const cv::Mat& b) {
    CV_DbgAssert(a.type() == CV_8UC1 && b.type() == CV_8UC1 && a.size() == b.size());
    unsigned acc = 0;
    for (int y = 0; y < a.rows; ++y) {
        const uint8_t* ra = a.ptr<uint8_t>(y);
        const uint8_t* rb = b.ptr<uint8_t>(y);
        for (int x = 0; x < a.cols; ++x)
            acc += static_cast<unsigned>(std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x])));
    }
    return static_cast<float>(acc) / static_cast<float>(a.rows * a.cols);
}

// The same, against the window of `img` with its top-left corner at (x, y) —
// without constructing a Mat header for that window. Sliding a patch over an
// image builds one header per candidate position, and at these sizes that is a
// large part of the overhead above. The CALLER guarantees the window is in
// bounds, which every hot loop already checks for its own reasons.
inline float patch_mad_at(const cv::Mat& img, int x, int y, const cv::Mat& patch) {
    CV_DbgAssert(img.type() == CV_8UC1 && patch.type() == CV_8UC1
                 && x >= 0 && y >= 0
                 && x + patch.cols <= img.cols && y + patch.rows <= img.rows);
    unsigned acc = 0;
    for (int r = 0; r < patch.rows; ++r) {
        const uint8_t* ri = img.ptr<uint8_t>(y + r) + x;
        const uint8_t* rp = patch.ptr<uint8_t>(r);
        for (int c = 0; c < patch.cols; ++c)
            acc += static_cast<unsigned>(std::abs(static_cast<int>(ri[c]) - static_cast<int>(rp[c])));
    }
    return static_cast<float>(acc) / static_cast<float>(patch.rows * patch.cols);
}

// The sum of absolute differences of the same window, abandoned as soon as the
// running sum reaches `bound`; the result is then some value >= bound.
//
// Every block-matching encoder does this: a search loop only needs to know
// whether a candidate beats the best so far, and most candidates are beaten
// within a few rows. Because a hot loop keeps the FIRST minimum (`<`, not
// `<=`), abandoning a candidate whose partial sum already reaches the best
// cannot change which offset wins -- the argmin and its value are the same
// integers the full sum gives, so callers stay bit-identical.
inline unsigned patch_sad_at_bounded(const cv::Mat& img, int x, int y,
                                     const cv::Mat& patch, unsigned bound) {
    CV_DbgAssert(img.type() == CV_8UC1 && patch.type() == CV_8UC1
                 && x >= 0 && y >= 0
                 && x + patch.cols <= img.cols && y + patch.rows <= img.rows);
    unsigned acc = 0;
    for (int r = 0; r < patch.rows; ++r) {
        const uint8_t* ri = img.ptr<uint8_t>(y + r) + x;
        const uint8_t* rp = patch.ptr<uint8_t>(r);
        for (int c = 0; c < patch.cols; ++c)
            acc += static_cast<unsigned>(std::abs(static_cast<int>(ri[c]) - static_cast<int>(rp[c])));
        if (acc >= bound) return acc;
    }
    return acc;
}

// Extract a BLOCK_SIZE×BLOCK_SIZE gray patch from a frame (bounds-checked).
// Returns an empty Mat if out of bounds.
inline cv::Mat extract_block(const cv::Mat& gray, int px, int py) {
    if (px < 0 || py < 0 ||
        px + BLOCK_SIZE > gray.cols ||
        py + BLOCK_SIZE > gray.rows)
        return {};
    return gray(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE));
}
