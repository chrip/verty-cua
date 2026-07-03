#include "session_processor.hpp"
#include <iostream>

SessionProcessor::SessionProcessor(std::string video_path, bool verbose)
    : video_path_(std::move(video_path)), verbose_(verbose)
{
    VideoSource probe(video_path_);
    if (!probe.ok()) return;
    frame_w_ = probe.width();
    frame_h_ = probe.height();
    fps_     = probe.fps();
    ok_ = frame_w_ > 0 && frame_h_ > 0;
}

// ── classify_transitions (session pass) ──────────────────────────────────────
// One full read of the video, classifying every frame-to-frame transition.
//
// The classifier is fed an ONLINE chrome estimate: pixel stability accumulated
// over the scroll transitions seen so far, reset at every page change. Without
// it, chrome shared by consecutive pages dilutes the changed-area fraction and
// a navigation between same-chrome pages goes undetected (measured on the
// multipage fixture: full-frame fraction 0.39 < 0.4 gate, interior 0.67).
// The per-page authoritative fixed masks are still built by each PageStitcher.
std::vector<Transition> SessionProcessor::classify_transitions()
{
    std::vector<Transition> transitions;

    VideoSource video(video_path_);
    if (!video.ok()) return transitions;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    if (!video.next(f)) return transitions;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();
    transitions.push_back({TransitionType::Identical, 0});   // frame 0 placeholder

    FixedAccumulator chrome(frame_w_, frame_h_);
    cv::Mat chrome_mask;

    counts_ = {};
    while (video.next(f)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
        Transition tr = classifier_.classify(prev_gray, curr_gray, chrome_mask);
        counts_.add(tr.type);
        transitions.push_back(tr);

        if (tr.type == TransitionType::Scroll) {
            chrome.accumulate(prev_gray, curr_gray);
            chrome_mask = chrome.mask();
        } else if (tr.type == TransitionType::PageChange) {
            chrome.reset();                // the new page's chrome is unknown
            chrome_mask = cv::Mat();
        }
        prev_gray = curr_gray.clone();
    }
    return transitions;
}

// ── run ───────────────────────────────────────────────────────────────────────
bool SessionProcessor::run()
{
    if (!ok_) return false;

    const std::vector<Transition> transitions = classify_transitions();
    frame_count_ = static_cast<long>(transitions.size());
    if (frame_count_ == 0) return false;

    const SessionSegmenter::Result seg = SessionSegmenter{}.segment(transitions);
    nav_frames_ = seg.navFrames;

    if (verbose_) {
        std::cout << "[session] frames=" << frame_count_
                  << "  transitions: identical=" << counts_.identical
                  << " scroll=" << counts_.scroll
                  << " pageChange=" << counts_.pageChange
                  << " unstable=" << counts_.unstable << "\n";
        std::cout << "[session] visits=" << seg.visits.size();
        for (const PageVisit& v : seg.visits)
            std::cout << "  [" << v.firstFrame << ".." << v.lastFrame << "]";
        std::cout << "\n";
    }

    pages_.clear();
    for (size_t i = 0; i < seg.visits.size(); ++i) {
        PageStitcher stitcher(video_path_, seg.visits[i],
                              frame_w_, frame_h_, fps_,
                              verbose_, static_cast<int>(i));
        PageResult page;
        page.visit        = seg.visits[i];
        page.image        = stitcher.run();
        page.docHeight    = stitcher.doc_height();
        page.fixedRects   = stitcher.fixed_rects();
        page.scrollEvents = stitcher.scroll_events();
        pages_.push_back(std::move(page));
    }

    return !pages_.empty();
}
