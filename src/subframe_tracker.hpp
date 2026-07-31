#pragma once
#include "types.hpp"
#include <string>
#include <vector>

// A SUBFRAME is a bounded region that changes while the document around it does
// not: an embedded prototype, a gallery, a tab switcher, a playing video ad. To
// the document model these are all the same event — "the page did not move but
// some of it is different" — and the pixel-temporal statistics of an ad and of a
// screen flow are, as far as the corpus can tell, identical: per-pixel change
// rate (19.5% vs 10.6%), longest run of consecutive changes (6 vs 4), longest
// still run (58 vs 60) all overlap, with a lab animation that must not be masked
// sitting between them on every one.
//
// What DOES separate them is whether the region ever holds still. A human
// clicking through a flow leaves each state on screen to be read; a video never
// stops. Measured, per region, as the share of in-place changes followed by a
// dwell of at least SUBFRAME_SETTLE_FRAMES:
//
//     region                              in-place changes   settled
//     lab fixture, animated panel                     300        0%
//     corpus, fixed video ad                           18        0%
//     real embedded phone screen (2 sessions)     93 / 116   31 / 39%
//     lab fixture, panel tapped 30 times               30      100%
//
// So a region with settled states carries content and each state is worth
// keeping; a region without them is noise. The two lab fixtures differ in
// nothing but this axis — same page, same panel, same geometry, one tapped and
// one playing (`--plan gallery` / `--plan gallery_video`).
//
// The tracker rides along the classification pass and needs no extra decode. It
// does not change any verdict: the noise mask still decides what the classifier
// ignores. What it adds is the answer to "and was any of that worth keeping".
class SubframeTracker {
public:
    struct State {
        long      frame = 0;     // first frame of the dwell: the settled appearance
        double    t     = 0.0;
        int       dwell = 0;     // transitions it held still for
        cv::Rect  box;           // what changed to produce it
        cv::Mat   image;         // the SUBFRAME rect of `frame`, filled afterwards
    };

    // An inner document of the subframe: a run of its own frames that it scrolled
    // through, stitched by the same pipeline one level down (see
    // subframe_stitcher.hpp). A subframe that only ever swapped screens has none.
    struct InnerDoc {
        long    first_frame = 0;
        long    last_frame  = 0;
        long    extent      = 0;
        int     scrolls     = 0;
        cv::Mat image;
    };

    struct Subframe {
        cv::Rect           rect;             // union of its states' boxes
        int                in_place_changes = 0;
        std::vector<long>  change_frames;    // when it changed (for per-page views)
        std::vector<State> states;           // settled appearances, in order
        std::vector<InnerDoc> inner;         // runs it scrolled through, stitched
        [[nodiscard]] bool is_screenflow() const {
            return static_cast<int>(states.size()) >= SUBFRAME_MIN_STATES;
        }
    };

    SubframeTracker(int frame_w, int frame_h, double fps, bool verbose);

    // Every transition of the pass, in order. `scrolled` marks transitions the
    // classifier explained as a document scroll: the whole frame moved, so
    // nothing about them is in-place.
    void observe(long frame, const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                 bool scrolled);

    // A state's image is the SUBFRAME's rectangle, not the box that happened to
    // change: one tap repaints the whole panel and the next only its body, so
    // cropping what changed would hand back inconsistent fragments of the same
    // region. The rectangle is only known once every change has been grouped, so
    // the owner fills the images afterwards, in frame order.
    [[nodiscard]] std::vector<long> state_frames_needed() const;
    void set_state_image(long frame, const cv::Mat& frame_bgr);

    // Drop states that do not differ from the one kept before them. Called once
    // the images exist, because "different appearance" is a question about pixels
    // and nothing earlier in the pipeline can answer it: two dwells can settle at
    // different frames, in different parts of a region, and still leave the region
    // looking exactly as it did.
    void drop_indistinct_states();

    // Close any dwell still open at the end of the video, then group the settled
    // states into subframes by overlap.
    void finish();

    [[nodiscard]] const std::vector<Subframe>& subframes() const { return subframes_; }
    [[nodiscard]] std::vector<Subframe>& subframes() { return subframes_; }

private:
    struct Pending {
        cv::Rect box;
        long     frame = 0;      // frame that first showed the new appearance
        double   t     = 0.0;
        int      dwell = 0;
        bool     open  = true;
    };

    void close(Pending& p);

    int    frame_w_, frame_h_;
    double fps_;
    bool   verbose_;
    std::vector<Pending>  pending_;   // dwells still being timed (a handful)
    std::vector<State>    settled_;   // dwells that lasted
    // Every in-place change with the frame it happened on: the frame is what lets
    // a session-wide region be reported per PAGE, since a region can outlive a
    // navigation (an ad on every page of a site) and its evidence then belongs to
    // more than one of them.
    std::vector<std::pair<cv::Rect, long>> change_boxes_;
    std::vector<Subframe> subframes_;
};
