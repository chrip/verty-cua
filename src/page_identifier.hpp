#pragma once
#include "types.hpp"
#include "page_stitcher.hpp"

// Result of matching a frame against a known page's canvas.
struct Identification {
    bool   matched    = false;
    long   vt0        = 0;     // document offset of the frame's viewport top
    double confidence = 0.0;   // fraction of usable sample blocks voting for vt0
};

// Recognizes whether a frame shows a page the session has already seen
// (Roadmap M4), by block voting of the frame against the page's stitched
// canvas — the same machinery as scroll detection, but frame-vs-document:
//
//   Sample textured blocks from the frame (the candidate page's fixed bands
//   are excluded — chrome matches any same-chrome page and carries no page
//   identity; near-uniform blocks are skipped — they match anywhere). Each
//   block searches its column of the candidate's canvas for the best-matching
//   vertical position; a good match votes for the implied document offset
//   vt0 = canvas_y − block_y. A dominant vote peak identifies the page AND
//   recovers the visit's entry scroll offset (browsers restore the scroll
//   position, so a revisit may enter mid-document).
class PageIdentifier {
public:
    // frame_gray: the visit's first stable frame (grayscale).
    // canvas_gray: the candidate page's current stitched canvas (grayscale).
    // interior: the candidate page's scroll area within the viewport.
    // noise_mask: confirmed persistent noise (video ads) — excluded from both
    //             voting and verification; ads carry no page identity.
    //
    // Two stages: block votes propose the offset vt0; a grid over the
    // frame↔canvas INTERSECTION then verifies it. Confidence is the fraction
    // of textured, non-noise grid blocks matching at vt0 — robust against
    // partial content change (images that popped in after the first visit).
    [[nodiscard]] Identification match(const cv::Mat& frame_gray,
                                       const cv::Mat& canvas_gray,
                                       const cv::Rect& interior,
                                       const cv::Mat& noise_mask = cv::Mat()) const;
};
