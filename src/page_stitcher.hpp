#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
#include "scrollbar_map.hpp"
#include "scrollbar_tracker.hpp"
#include "transition_classifier.hpp"
#include "video_source.hpp"
#include <cmath>
#include <string>
#include <vector>

// How a fixed region is anchored in the viewport. Scrollbar is the browser's
// own vertical bar — viewport chrome rather than page content, and detected
// outright rather than inferred from pixel stability, so its extent is exact.
enum class FixedKind { Header, Footer, Left, Right, Scrollbar };

struct FixedRect {
    cv::Rect  rect;   // bounding box in viewport coordinates
    FixedKind kind;
};

// One detected scroll step (from the mask-aware pass).
struct ScrollEvent {
    long   frame = 0;   // GLOBAL frame index at which the viewport reached vt
    double t     = 0.0; // seconds since video start (frame / fps)
    int    dy    = 0;   // scroll delta vs previous frame (positive = down)
    long   vt    = 0;   // viewport top in document coordinates after the step
};

// Reconstructs the full-page screenshot of ONE page, assembled from one or
// more visits (a visit is a contiguous stable frame range; a REVISIT of the
// same page merges into the same stitcher — Roadmap M4).
//
// Per visit, two analysis passes over the visit's frames:
//   1. analyze_visit  — accumulate the position-fixed pixel mask (shared
//                       across all visits of the page) and a rough per-frame
//                       scroll offset, then refresh the fixed-band rectangles.
//   2. refine_visit   — re-detect the scroll offsets with the fixed areas
//                       excluded from the search; authoritative offsets.
//
// A visit enters the document at vt_start: 0 for the first visit, or the
// entry offset recovered by the PageIdentifier for a revisit (browsers
// restore the scroll position, so a revisit may start mid-document).
//
// reconstruct() merges all visits into one canvas: every visit's scrolling
// interior is copied at its absolute offsets, then the fixed bands are
// stamped on top (header at the top, footer at the bottom, sidebars spanning
// the full document height).
class PageStitcher {
public:
    // scrollbar: located vertical scrollbar of the session, or nullptr. Used to
    // calibrate this page's thumb→offset map, which lets a later visit be
    // re-localized after a jump that content matching cannot follow.
    PageStitcher(std::string video_path, int frame_w, int frame_h, double fps,
                 bool verbose = false, int page_id = 0,
                 const ScrollbarTracker* scrollbar = nullptr);

    // Analyze one visit of this page (runs both analysis passes).
    //
    // bridged marks a visit that CONTINUES the previous one after a jump too
    // large for frame-to-frame matching: it emits the jump as one large scroll
    // event, because that is what it was. vt_uncertainty is the declared error
    // of vt_start (0 when the offset came from content, non-zero when it was
    // re-localized from the scrollbar).
    void add_visit(const PageVisit& visit, long vt_start,
                   bool bridged = false, long vt_uncertainty = 0);

    // Merge all visits into the page canvas (callable after every add_visit).
    [[nodiscard]] cv::Mat reconstruct();

    // ── Page state (valid after add_visit / reconstruct) ────────────────────
    struct VisitTrace {
        PageVisit         visit;
        long              vt_start = 0;
        long              vt_uncertainty = 0;   // declared error of vt_start (px)
        bool              bridged  = false;     // entered by a re-localized jump
        std::vector<long> vt_by_frame;   // absolute doc coords per LOCAL index
    };

    // A document row range for which NO SCROLLING CONTENT was ever observed,
    // and which therefore cannot be reconstructed at all. Two causes, both
    // permanent: a jump skipped the rows outright (they are blank in the
    // stitch), or a viewport-fixed element covered them at every offset the
    // page can be scrolled to — a full-width sticky nav hides the document
    // rows under it forever, and the stitch shows the stamped element there
    // instead of page content. Declared rather than silently passed off as
    // reconstructed content.
    struct Gap {
        long from = 0;   // inclusive
        long to   = 0;   // exclusive
    };

    [[nodiscard]] const std::vector<VisitTrace>&  visits() const { return visits_; }
    [[nodiscard]] const std::vector<FixedRect>&   fixed_rects() const { return rects_; }
    [[nodiscard]] const std::vector<ScrollEvent>& scroll_events() const { return scroll_events_; }
    // Interior viewport-fixed elements (floating buttons etc.), viewport coords.
    [[nodiscard]] const std::vector<cv::Rect>& overlays() const { return overlays_; }
    // Confirmed persistent-noise mask (fixed video ads), viewport coords.
    [[nodiscard]] const cv::Mat& noise_mask() const { return noise_mask_; }
    // How much document this page's frames actually COVERED — deepest observed
    // offset plus one viewport. NOT the document's height: a session that
    // stopped early never saw the rest, and this number cannot know that. The
    // document's own height, where the scrollbar could be read, is a separate
    // and separately-declared estimate (document_height_estimate).
    [[nodiscard]] long captured_extent() const { return max_vt_ + frame_h_; }

    // The document's height per this page's calibrated scrollbar map, with the
    // error it is entitled to. 0 / 0 when no map was calibrated.
    [[nodiscard]] long document_height_estimate() const { return sb_map_.document_height(); }
    [[nodiscard]] long document_height_uncertainty() const { return sb_map_.document_height_uncertainty(); }

    // Measured drift of the dead-reckoned offsets against the scrollbar's
    // absolute reference: the worst disagreement among the anchors the map
    // accepted. -1 when there is no independent reference to measure against.
    [[nodiscard]] long drift_px() const {
        return sb_map_.valid() ? static_cast<long>(std::lround(sb_map_.residual())) : -1;
    }

    // Did the session reach the document's end? 1 yes, 0 no, -1 unknowable
    // (no scrollbar, so no independent notion of "the end").
    [[nodiscard]] int reached_bottom() const {
        const long est = document_height_estimate();
        if (est <= 0) return -1;
        return captured_extent() + document_height_uncertainty() >= est ? 1 : 0;
    }
    // Document rows never observed (valid after reconstruct()).
    [[nodiscard]] const std::vector<Gap>& gaps() const { return gaps_; }
    // This page's calibrated scrollbar map (invalid if never calibrated).
    [[nodiscard]] const ScrollbarMap& scrollbar_map() const { return sb_map_; }

private:
    std::string video_path_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;
    int         page_id_;

    TransitionClassifier classifier_;
    FixedAccumulator     accumulator_;           // chrome evidence, all visits
    DynamicAccumulator   noise_accumulator_;     // persistent-noise evidence
    const ScrollbarTracker* scrollbar_ = nullptr;   // session-wide, not owned
    ScrollbarMap         sb_map_;                // this page's thumb→offset map
    std::vector<Gap>     gaps_;                  // never-observed document rows
    std::vector<VisitTrace> visits_;
    long max_vt_ = 0;                    // deepest viewport top over all visits
    long min_vt_ = -1;                   // shallowest (-1 = none yet)
    std::vector<ScrollEvent> scroll_events_;

    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat noise_mask_;       // confirmed persistent noise (video ads)
    cv::Mat first_frame_;      // frame at the shallowest vt — source for header/sidebars
    cv::Mat last_frame_;       // frame at the deepest vt    — source for the footer
    int     sb_w_ = 0;         // scrollbar columns at the right edge (0 = none)
    cv::Mat sb_clean_;         // thumb-free scrollbar strip (per-pixel max, all visits)

    // ── Incremental fold state (see reconstruct()) ───────────────────────────
    // The scrolling content is accumulated ONCE per visit and kept, so that
    // reconstruct() — which the registry calls after every visit — folds only
    // what is new instead of re-reading the whole page. The chrome is not part
    // of it: stamps depend on the current document height and are re-applied
    // per call.
    cv::Mat              content_;       // folded scroll content, document coords
    cv::Mat              written_;       // per-pixel written map, interior columns
    std::vector<uint8_t> row_written_;   // any content pixel in this document row
    size_t               folded_ = 0;    // visits already folded into content_
    cv::Rect             folded_interior_;   // geometry the fold was made under
    std::vector<cv::Rect> folded_overlays_;

    std::vector<FixedRect> rects_;
    std::vector<cv::Rect>  overlays_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    // Opens the video positioned at the given visit's first frame.
    [[nodiscard]] VideoSource open_at(const PageVisit& v) const;

    void analyze_visit(const PageVisit& v, long vt_start);   // pass 1
    void refine_visit(const PageVisit& v, long vt_start,     // pass 2
                      bool bridged, long vt_uncertainty);

    void extract_rects();
    void extract_overlays();
    void add_overlay(const cv::Rect& o);   // merge into the page-wide overlay list
    void append_scroll_events(const VisitTrace& trace);
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;
    void stamp_scrollbar(cv::Mat& canvas, long doc_h) const;
};
