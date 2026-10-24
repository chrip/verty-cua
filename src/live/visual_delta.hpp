#pragma once
#include <opencv2/opencv.hpp>

#include "perception/transition_classifier.hpp"

// What one frame pair looks like, measured cheaply and without interpretation.
//
// This is the System-0/System-1 output that everything above it consumes: the
// M0 replay tool reports it, and the M1 verifier reasons over it. Both use the
// SAME struct on purpose -- the numbers the verifier's thresholds were
// fitted to are the numbers this reports, and a second implementation would
// let those two drift apart silently.
struct VisualDelta {
    // ── System 0: which pixels changed ──────────────────────────────────────
    double   changed_frac = 0.0;   // share of pixels past SAD_THRESHOLD

    // The same share, counting only pixels whose absolute difference is large.
    //
    // SAD_THRESHOLD is a detection threshold, not a significance one: a page
    // that dims from white to light grey when it loses focus, or an overlay
    // washing over content, crosses it across a huge area at an amplitude a
    // human cannot see in a thumbnail. Measured on one step: 53% of its
    // "changed" pixels differ by under 30 levels, and requiring >= 60 takes
    // changed_frac from 0.0364 to 0.0126 -- across the boundary between two
    // extent classes, purely by discounting a wash.
    //
    // Reported, not yet used to classify: switching the extent thresholds onto
    // it needs labels, not just a plausible story.
    double   changed_frac_strong = 0.0;
    cv::Rect bbox;                 // bounding box of all changed pixels
    double   bbox_frac = 0.0;      // that box as a share of the frame
    int      components = 0;       // changed regions after a 9x9 close
    int      bands_changed = 0;    // horizontal bands past BAND_CHANGED_MIN_FRAC
    int      bands_total = 0;

    // ── System 1: what kind of change the inherited classifier calls it ─────
    // Kept for its scroll detection, which is the one judgement in it that M0
    // did not contradict. Its Identical/Unstable/PageChange split must NOT be
    // used to decide whether something happened: it conflates "nothing
    // changed" with "a local UI change happened".
    TransitionType transition = TransitionType::Identical;
    int            dy = 0;         // meaningful only when transition == Scroll

    int frame_w = 0, frame_h = 0;
};

// Locality of the change relative to a point -- the action's coordinate.
struct ChangeLocality {
    bool   valid = false;          // false when nothing changed
    double frac_within_r = 0.0;    // share of changed pixels within the radius

    // Distance to the nearest pixel that actually changed. This is the honest
    // locality measure and the one the verifier uses.
    double dist_to_change = 0.0;

    // Distance to the UNION bounding box of every changed region, and whether
    // the point falls inside it. Kept because M0 reported them, but they are
    // treacherous: a change with several disjoint components -- a title-bar
    // marker plus an edited text line, say -- has a union box spanning most of
    // the frame, so `inside_bbox` is true for points nowhere near any change.
    // Measured on the corpus: this produced a false Expected on a click that
    // landed on the ruler while the real changes were the window title and a
    // text line 200px away. Do not verify with these.
    double dist_to_bbox = 0.0;
    bool   inside_bbox = false;
};

// The System-0 changed mask: absdiff -> 3x3 blur -> SAD_THRESHOLD, CV_8U.
cv::Mat change_mask(const cv::Mat& a, const cv::Mat& b);

// Both frames must be single-channel and the same size.
VisualDelta measure_delta(const cv::Mat& before_gray, const cv::Mat& after_gray,
                          TransitionClassifier& classifier);

ChangeLocality measure_locality(const cv::Mat& before_gray, const cv::Mat& after_gray,
                                cv::Point at, int radius, const VisualDelta& d);
