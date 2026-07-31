#include "subframe_tracker.hpp"
#include <algorithm>
#include <iostream>

namespace {

double iou(const cv::Rect& a, const cv::Rect& b)
{
    const double inter = (a & b).area();
    const double uni   = a.area() + b.area() - inter;
    return uni > 0 ? inter / uni : 0.0;
}

int median_of(std::vector<int> v)
{
    if (v.empty()) return 0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}   // namespace

SubframeTracker::SubframeTracker(int frame_w, int frame_h, double fps, bool verbose)
    : frame_w_(frame_w), frame_h_(frame_h), fps_(fps), verbose_(verbose) {}

// ── observe ───────────────────────────────────────────────────────────────────
// One transition, in three steps: find the bounded regions that changed while the
// document stood still, end any dwell one of them disturbed, and start a dwell for
// each of them.
//
// The regions are connected COMPONENTS of the changed mask, not its bounding box.
// A bounding box spans everything that changed at once, so a screen swap that also
// repaints a button 1200px away reads as one 1550px-wide region — measured on a
// real recording, where it merged an embedded phone screen with the host page's
// buttons and the two then could not be told apart.
void SubframeTracker::observe(long frame, const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                              bool scrolled)
{
    const double frame_area = static_cast<double>(frame_w_) * frame_h_;
    std::vector<cv::Rect> boxes;
    cv::Mat changed;
    int changed_px = 0;

    // A document scroll moves everything, so it can neither open nor end a dwell:
    // the subframe's pixels moved with the page, not on their own.
    if (!scrolled) {
        cv::Mat diff, smooth;
        cv::absdiff(prev_gray, curr_gray, diff);
        cv::blur(diff, smooth, cv::Size(3, 3));
        cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
        changed_px = cv::countNonZero(changed);

        // Components are only worth computing for a transition that could hold a
        // subframe change at all: something changed, and not the whole viewport.
        if (changed_px >= SUBFRAME_MIN_CHANGED_PX
            && changed_px <= SUBFRAME_MAX_AREA_FRAC * frame_area) {
            // Open to kill speckle, then CLOSE generously. Closing matters as much
            // as the components do: a region that SCROLLS its own content changes
            // only the boundaries between its rows, so its changed mask is a set of
            // thin strips of a few thousand pixels each — every one of them below
            // the floor, and the region goes undetected entirely (measured on the
            // inner-scroll fixture: 451 frames of a panel scrolling 2286px, zero
            // regions found). Closing merges strips that belong to one region while
            // leaving two regions a screen apart separate.
            cv::Mat opened;
            cv::morphologyEx(changed, opened, cv::MORPH_OPEN,
                             cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
            cv::morphologyEx(opened, opened, cv::MORPH_CLOSE,
                             cv::getStructuringElement(cv::MORPH_RECT,
                                                       cv::Size(SUBFRAME_CLOSE_PX, SUBFRAME_CLOSE_PX)));
            cv::Mat labels, stats, centroids;
            const int n = cv::connectedComponentsWithStats(opened, labels, stats, centroids, 8);
            for (int k = 1; k < n; ++k) {
                if (stats.at<int>(k, cv::CC_STAT_AREA) < SUBFRAME_MIN_CHANGED_PX) continue;
                const cv::Rect b(stats.at<int>(k, cv::CC_STAT_LEFT),
                                 stats.at<int>(k, cv::CC_STAT_TOP),
                                 stats.at<int>(k, cv::CC_STAT_WIDTH),
                                 stats.at<int>(k, cv::CC_STAT_HEIGHT));
                if (b.area() <= SUBFRAME_MAX_AREA_FRAC * frame_area) boxes.push_back(b);
            }
        }
    }

    // Any open dwell whose own region moved again is over.
    for (Pending& p : pending_) {
        if (!p.open) continue;
        bool disturbed = scrolled;
        if (!disturbed && changed_px > 0)
            disturbed = cv::countNonZero(changed(p.box)) >= SUBFRAME_MIN_CHANGED_PX / 4;
        if (disturbed) close(p);
        else           ++p.dwell;
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [](const Pending& p) { return !p.open; }),
                   pending_.end());

    for (const cv::Rect& b : boxes) {
        change_boxes_.push_back(b);
        Pending p;
        p.box   = b;
        p.frame = frame;          // this frame already shows the new appearance
        p.t     = fps_ > 0 ? static_cast<double>(frame) / fps_ : 0.0;
        pending_.push_back(std::move(p));
    }
}

// A dwell ends. It becomes a STATE only if it lasted — otherwise the appearance
// was one frame of something in motion.
void SubframeTracker::close(Pending& p)
{
    p.open = false;
    if (p.dwell < SUBFRAME_SETTLE_FRAMES) return;
    State s;
    s.frame = p.frame;
    s.t     = p.t;
    s.dwell = p.dwell;
    s.box   = p.box;
    settled_.push_back(std::move(s));
}

// ── finish ────────────────────────────────────────────────────────────────────
// Group the changes into subframes. Grouping is by OVERLAP RATIO against each
// group's median box, not by "do the boxes touch": a chain of touching boxes
// swallows the whole viewport (measured — one group grew to 1800×956 of a
// 1918×956 frame), while a ratio keeps a gallery and a ticker apart even when
// they are adjacent.
void SubframeTracker::finish()
{
    for (Pending& p : pending_)
        if (p.open) close(p);
    pending_.clear();

    const double frame_area = static_cast<double>(frame_w_) * frame_h_;

    // 1. Candidate regions: changes that recur at the same place. Grouping is by
    //    OVERLAP RATIO against each group's median box, not by "do the boxes
    //    touch" — a chain of touching boxes swallows the viewport (measured: one
    //    group grew to 1800×956 of a 1918×956 frame).
    struct Group { std::vector<cv::Rect> boxes; cv::Rect median; };
    std::vector<Group> groups;
    auto remedian = [](Group& g) {
        std::vector<int> xs, ys, ws, hs;
        for (const cv::Rect& b : g.boxes) { xs.push_back(b.x); ys.push_back(b.y);
                                            ws.push_back(b.width); hs.push_back(b.height); }
        g.median = cv::Rect(median_of(xs), median_of(ys), median_of(ws), median_of(hs));
    };
    for (const cv::Rect& b : change_boxes_) {
        Group* best = nullptr;
        double best_iou = SUBFRAME_MIN_IOU;
        for (Group& g : groups) {
            const double o = iou(g.median, b);
            if (o >= best_iou) { best_iou = o; best = &g; }
        }
        if (!best) { groups.push_back({{b}, b}); continue; }
        best->boxes.push_back(b);
        remedian(*best);
    }

    // 2. A subframe is SUBSTANTIAL and RECURRING. Without the size floor, every
    //    text block of a page that reloads its own content qualifies: measured on
    //    the auto-refreshing news portal, 79 "subframes" of 0.3-3% of the frame,
    //    several of them with two settled states. The regions this is for are
    //    7-18% of the frame (lab panel 7%, embedded phone screen 12%, fixed video
    //    ad 18%).
    std::vector<cv::Rect> regions;
    for (const Group& g : groups)
        if (static_cast<int>(g.boxes.size()) >= SUBFRAME_MIN_CHANGES
            && g.median.area() >= SUBFRAME_MIN_AREA_FRAC * frame_area)
            regions.push_back(g.median);
    // Largest first, so a region that is part of a bigger one folds into it.
    std::sort(regions.begin(), regions.end(),
              [](const cv::Rect& a, const cv::Rect& b) { return a.area() > b.area(); });
    std::vector<cv::Rect> kept;
    for (const cv::Rect& r : regions) {
        bool inside = false;
        for (const cv::Rect& k : kept)
            if ((k & r).area() >= SUBFRAME_CONTAINED_FRAC * r.area()) { inside = true; break; }
        if (!inside) kept.push_back(r);
    }

    // 3. Everything that happened INSIDE a region belongs to it. Containment, not
    //    overlap ratio: a screen swap repaints all of the region on one tap and a
    //    corner of it on the next, so its changes have wildly different boxes and
    //    an overlap ratio scatters them (measured: an embedded phone screen kept
    //    2 of its 29 settled states that way).
    for (const cv::Rect& r : kept) {
        Subframe sf;
        sf.rect = r;
        for (const cv::Rect& b : change_boxes_)
            if ((r & b).area() >= SUBFRAME_CONTAINED_FRAC * b.area()) ++sf.in_place_changes;
        for (const State& s : settled_)
            if ((r & s.box).area() >= SUBFRAME_CONTAINED_FRAC * s.box.area()) sf.states.push_back(s);
        std::sort(sf.states.begin(), sf.states.end(),
                  [](const State& a, const State& b) {
                      if (a.frame != b.frame) return a.frame < b.frame;
                      return a.box.area() > b.box.area();
                  });
        // One tap can repaint a region as several components — a title bar and a
        // body — and each opens its own dwell. They are one appearance, so the
        // largest box of a frame wins and the rest are dropped (measured: the lab
        // panel reported 40 states for 30 taps before this).
        sf.states.erase(std::unique(sf.states.begin(), sf.states.end(),
                                    [](const State& a, const State& b) {
                                        return a.frame == b.frame;
                                    }),
                        sf.states.end());
        subframes_.push_back(std::move(sf));
    }
    std::sort(subframes_.begin(), subframes_.end(),
              [](const Subframe& a, const Subframe& b) { return a.rect.y < b.rect.y; });

    if (verbose_) {
        for (const Subframe& sf : subframes_)
            std::cout << "[subframe] " << sf.rect.width << "×" << sf.rect.height
                      << " at (" << sf.rect.x << "," << sf.rect.y << ")  "
                      << sf.in_place_changes << " in-place changes, "
                      << sf.states.size() << " settled states → "
                      << (sf.is_screenflow() ? "SCREEN FLOW (states kept)"
                                             : "dynamic noise (nothing kept)") << "\n";
    }
}

// ── state images ──────────────────────────────────────────────────────────────
std::vector<long> SubframeTracker::state_frames_needed() const
{
    std::vector<long> out;
    for (const Subframe& sf : subframes_) {
        if (!sf.is_screenflow()) continue;      // noise keeps no state
        for (const State& s : sf.states) out.push_back(s.frame);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void SubframeTracker::set_state_image(long frame, const cv::Mat& frame_bgr)
{
    if (frame_bgr.empty()) return;
    const cv::Rect bounds(0, 0, frame_bgr.cols, frame_bgr.rows);
    for (Subframe& sf : subframes_) {
        if (!sf.is_screenflow()) continue;
        const cv::Rect r = sf.rect & bounds;
        if (r.area() <= 0) continue;
        for (State& s : sf.states)
            if (s.frame == frame && s.image.empty()) s.image = frame_bgr(r).clone();
    }
}
