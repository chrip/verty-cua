#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
#include "page_registry.hpp"
#include "session_segmenter.hpp"
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
    struct PageResult {
        cv::Mat image;
        long    docHeight = 0;
        std::vector<FixedRect>   fixedRects;
        std::vector<cv::Rect>    overlays;                // interior fixed elements
        std::vector<ScrollEvent> scrollEvents;            // global frames / timestamps
        std::vector<PageStitcher::VisitTrace> visits;     // all visits of this page
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
    [[nodiscard]] const TransitionCounts& transition_counts() const { return counts_; }

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

    // Session pass: classification of the transition into every frame
    // (index 0 is a placeholder). seed_noise pre-loads the persistent-noise
    // mask so a fixed video ad is masked from frame one — see run() for the
    // two-pass reason. On return, noise_mask_ holds the matured mask.
    [[nodiscard]] std::vector<Transition> classify_transitions(
        const cv::Mat& seed_noise = cv::Mat());

    cv::Mat noise_mask_;   // matured persistent-noise mask (last classify pass)

    // The visit's first stable frame, grayscale (for page identification).
    [[nodiscard]] cv::Mat read_frame_gray(long index) const;
};
