#pragma once
#include "perception/types.hpp"
#include "perception/transition_classifier.hpp"
#include <string>
#include <vector>

// Runs the pipeline INSIDE a subframe.
//
// A subframe that scrolls its own content is a document, and the document model
// this project is built on already knows what to do with one — classify its
// transitions, track its offset, reconcile its rows, stitch it. The only thing
// that was in the way is that all of it assumed "frame" meant the whole frame.
// It does not have to: `VideoSource` takes a rectangle, so the same
// TransitionClassifier and the same PageStitcher read a smaller frame and mean
// exactly the same things one level down. That is all this class is — the
// recursion, with the bookkeeping to decide where one inner document ends and
// the next begins.
//
// Where it ends is decided by the inner classifier itself: a transition it calls
// PAGE_CHANGE is a new screen inside the subframe (an embedded prototype tapped
// to its next step), while SCROLL and IDENTICAL both continue the one it is on.
// A run that never scrolled needs no stitch: its single settled frame already
// shows all of it, and stitching would cost three more passes over the video to
// reproduce a crop the tracker already has.
class SubframeStitcher {
public:
    struct Doc {
        long    first_frame = 0;
        long    last_frame  = 0;
        long    extent      = 0;   // rows of inner document the frames covered
        int     scrolls     = 0;   // inner scroll transitions
        cv::Mat image;             // the stitch, taller than the subframe
    };

    SubframeStitcher(std::string video_path, double fps, bool verbose);

    // Inner documents of `roi` over [first_frame, last_frame] that SCROLLED.
    // Returns them in order; a run without a scroll is deliberately absent.
    // `doc_moved[frame]` says the DOCUMENT's own offset changed on the transition
    // into that frame. Those transitions end the current inner run: the subframe's
    // rectangle is fixed in frame coordinates, so while the page moves that
    // rectangle is looking at page content sliding past, not at the subframe's own
    // document — and the inner classifier cannot tell the two apart. Measured
    // without this: the fixture's inner document came out 476px too tall (of
    // 2502), and a news portal's article read as a 4085px inner document.
    [[nodiscard]] std::vector<Doc> run(const cv::Rect& roi, long first_frame, long last_frame,
                                       int subframe_id,
                                       const std::vector<bool>& doc_moved);

private:
    std::string video_path_;
    double      fps_;
    bool        verbose_;
};
