#pragma once
#include "perception/scrollbar_tracker.hpp"
#include <vector>

// Maps a scrollbar thumb position to a DOCUMENT offset, for one page.
//
// The map is linear — vt = a·thumb_top + b — and it is CALIBRATED from the
// page's own content-tracked scrolls: every frame whose offset is already known
// (because the incremental frame-to-frame chain was intact) contributes an
// anchor, and the anchors fix a and b. Nothing about the platform's scrollbar
// metrics — arrow height, track insets, minimum thumb size — has to be known or
// assumed, which is what makes this work across browsers and themes.
//
// The slope also has a closed form from geometry, a = viewport_h / thumb_h
// (thumb_h / track_h = viewport_h / docHeight, so one thumb pixel is worth one
// viewport height of document per thumb height). That serves two purposes: it
// is the fallback when the anchors are too clustered to fit a slope, and it is
// the sanity gate that a mis-detected column cannot pass.
//
// The thumb HEIGHT is the map's fingerprint. It depends on the document height
// alone, so it is invariant to scroll position and says WHICH document the map
// describes — the one piece of page identity that survives a jump with zero
// content overlap. Readings with a different thumb height are a different
// document (or the same one after lazy-loading grew it) and never enter the fit.
class ScrollbarMap {
public:
    explicit ScrollbarMap(int frame_h);

    // Add an anchor: a reading whose document offset is known from content
    // tracking. Ignored unless the reading is valid.
    //
    // Returns false when the anchor CONTRADICTS an already-valid map. Anchors
    // arrive in frame order, and a visit's offsets are correct until the
    // incremental chain breaks — so a late anchor that disagrees means the
    // chain broke (a jump was missed), not that the map is wrong. Such anchors
    // are refused rather than averaged in, which keeps the map anchored to the
    // trustworthy prefix; their count is itself evidence that offsets after
    // that point need re-localizing.
    bool add(const ScrollbarReading& r, long vt);

    [[nodiscard]] int rejected() const { return rejected_; }

    [[nodiscard]] bool   valid() const { return valid_; }
    [[nodiscard]] int    fingerprint() const { return fingerprint_; }  // thumb height
    [[nodiscard]] int    points() const { return used_; }              // anchors in the fit
    [[nodiscard]] double residual() const { return residual_; }        // worst anchor error, px
    [[nodiscard]] double slope() const { return a_; }
    [[nodiscard]] bool   slope_fitted() const { return slope_fitted_; }

    // Document offset of a frame showing the thumb at this position.
    [[nodiscard]] long predict(int thumb_top) const;

    // Uncertainty of a predicted offset. Floor: the worst calibration residual,
    // but never finer than one thumb pixel — the thumb's edges are hard, so
    // there is no sub-pixel information to be had and the quantum is
    // irreducible (measured 13.0 document px per thumb px on rw4's deep page).
    // Plus a slope term proportional to how far the prediction reaches beyond
    // the calibrated range, because the slope itself is only known to
    // SB_SLOPE_ERR_FRAC.
    [[nodiscard]] long uncertainty(long predicted) const;

    // Does this reading describe the same document as the map?
    [[nodiscard]] bool matches(const ScrollbarReading& r) const;

    // ── The document's own height, which nothing else here can measure ──────
    // A session that stopped scrolling early cannot tell from its own frames
    // how much document it never reached. The calibrated map can: the track
    // geometry falls out of a and b (see SB_TRACK_SYMMETRY_PX). 0 if invalid.
    [[nodiscard]] long document_height() const;
    // Declared error of that estimate: the map's own uncertainty at the deepest
    // reachable offset, plus the cost of assuming the two arrow buttons are the
    // same size.
    [[nodiscard]] long document_height_uncertainty() const;

private:
    void refit();

    struct Anchor {
        int  thumb_top = 0;
        int  thumb_h   = 0;
        long vt        = 0;
    };

    int                 frame_h_;
    std::vector<Anchor> anchors_;
    int                 fingerprint_  = 0;
    bool                valid_        = false;
    bool                slope_fitted_ = false;
    int                 used_         = 0;
    int                 rejected_     = 0;
    double              a_ = 0.0, b_ = 0.0, residual_ = 0.0;
};
