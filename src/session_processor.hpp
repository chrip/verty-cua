#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
#include "page_registry.hpp"
#include "scrollbar_tracker.hpp"
#include "session_segmenter.hpp"
#include "subframe_tracker.hpp"
#include <memory>
#include <string>
#include <vector>

// Orchestrates a whole session (Roadmap M3 + M4):
//
//   1. Classify every frame transition (session pass with an online chrome
//      estimate, reset at each page change).
//   2. Segment the transition stream into page visits (SessionSegmenter).
//   3. Assign every visit to a page (PageRegistry): a visit whose first frame
//      matches a known page's canvas is a REVISIT and merges into that page's
//      stitch at the recovered entry offset; otherwise it starts a new page.
class SessionProcessor {
public:
    explicit SessionProcessor(std::string video_path, bool verbose = false,
                              Sensitivity sensitivity = Sensitivity::Normal);

    [[nodiscard]] bool ok() const { return ok_; }

    // Runs the whole pipeline; returns false if nothing could be processed.
    [[nodiscard]] bool run();

    // One reconstructed DISTINCT page (valid after run()).
    // A subframe as it belongs to ONE page: the region and its verdict are
    // properties of the region (decided on all the evidence in the session,
    // because a region that outlives a navigation is one region), while the
    // states, the inner documents and the change count are this page's share of
    // it. A page can hold several — an ajax gallery and a scrollable comment
    // section under it are two — and one region can appear under several pages.
    struct PageSubframe {
        cv::Rect                              rect;
        bool                                  screenflow = false;
        int                                   in_place_changes = 0;
        std::vector<SubframeTracker::State>   states;
        std::vector<SubframeTracker::InnerDoc> inner;
    };

    struct PageResult {
        cv::Mat image;
        long    capturedExtent = 0;   // deepest observed offset + one viewport
        long    documentHeight = 0;   // scrollbar estimate, 0 = unknown
        long    documentHeightUncertainty = 0;
        long    driftPx = -1;         // measured dead-reckoning drift, -1 = unknown
        int     reachedBottom = -1;   // 1 yes / 0 no / -1 unknowable
        std::vector<FixedRect>   fixedRects;
        std::vector<cv::Rect>    overlays;                // interior fixed elements
        std::vector<ScrollEvent> scrollEvents;            // global frames / timestamps
        std::vector<PageStitcher::VisitTrace> visits;     // all visits of this page
        std::vector<PageStitcher::Gap> gaps;              // never-observed doc rows
        std::vector<PageSubframe> subframes;              // regions that changed in place
    };

    // One frame of the session, with the answer to "which page, and where in
    // it?" — or an explicit reason why there is no answer. The claim this tool
    // makes is per FRAME, so every frame appears here: a scroll-event list alone
    // silently omits the frames that belong to no visit (measured: up to a
    // quarter of a real session).
    struct FrameRecord {
        long        frame = 0;
        double      t = 0.0;
        int         pageId = -1;      // -1 = not attributed to any page
        long        vt = -1;          // -1 = no offset known
        std::string reason;           // "" when localized, else why not
    };

    // One navigation between visits.
    struct NavEvent {
        long        frame = 0;        // first stable frame of the new visit
        int         from = 0, to = 0; // page ids
        std::string type;             // "NAVIGATE" | "REVISIT"
        double      confidence = 0.0; // identifier vote fraction (REVISIT)
        long        vtStart = 0;      // entry scroll offset (REVISIT)
    };

    // ── Session outputs (valid after run()) ─────────────────────────────────
    [[nodiscard]] const std::vector<PageResult>& pages() const { return pages_; }
    [[nodiscard]] const std::vector<NavEvent>& navigations() const { return navigations_; }
    [[nodiscard]] const std::vector<FrameRecord>& frames() const { return frames_; }
    [[nodiscard]] const TransitionCounts& transition_counts() const { return counts_; }
    // Regions that changed while the document did not, with the states of the
    // ones that settled (see subframe_tracker.hpp).
    [[nodiscard]] const std::vector<SubframeTracker::Subframe>& subframes() const {
        return subframes_;
    }

    [[nodiscard]] long   frame_count() const { return frame_count_; }
    [[nodiscard]] int    frame_w() const { return frame_w_; }
    [[nodiscard]] int    frame_h() const { return frame_h_; }
    [[nodiscard]] double fps() const { return fps_; }

private:
    std::string video_path_;
    bool        verbose_;
    bool        ok_          = false;
    int         frame_w_     = 0;
    int         frame_h_     = 0;
    double      fps_         = 0.0;
    long        frame_count_ = 0;

    Sensitivity             sensitivity_;
    TransitionClassifier    classifier_;
    TransitionCounts        counts_;       // session-level (segmentation pass)
    std::vector<PageResult> pages_;
    std::vector<NavEvent>   navigations_;
    std::vector<FrameRecord> frames_;      // one entry per frame of the video

    // Session pass: classification of the transition into every frame
    // (index 0 is a placeholder). seed_noise pre-loads the persistent-noise
    // mask so a fixed video ad is masked from frame one — see run() for the
    // two-pass reason. On return, noise_mask_ holds the matured mask.
    [[nodiscard]] std::vector<Transition> classify_transitions(
        const cv::Mat& seed_noise = cv::Mat());

    cv::Mat noise_mask_;
    std::vector<SubframeTracker::Subframe> subframes_;   // matured persistent-noise mask (last classify pass)

    // The session's vertical scrollbar, located during the classification pass.
    // Absolute-offset evidence for jumps no content matching can follow; shared
    // (unowned) with the registry and every page stitcher.
    std::unique_ptr<ScrollbarTracker> scrollbar_;

    // The visit's first stable frame, grayscale (for page identification).
    [[nodiscard]] cv::Mat read_frame_gray(long index) const;
};
