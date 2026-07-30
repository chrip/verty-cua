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

// Extract a BLOCK_SIZE×BLOCK_SIZE gray patch from a frame (bounds-checked).
// Returns an empty Mat if out of bounds.
inline cv::Mat extract_block(const cv::Mat& gray, int px, int py) {
    if (px < 0 || py < 0 ||
        px + BLOCK_SIZE > gray.cols ||
        py + BLOCK_SIZE > gray.rows)
        return {};
    return gray(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE));
}
