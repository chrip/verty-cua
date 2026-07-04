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

// ── read_frame_gray ───────────────────────────────────────────────────────────
cv::Mat SessionProcessor::read_frame_gray(long index) const
{
    VideoSource video(video_path_);
    VideoSource::Frame f;
    if (!video.ok() || !video.skip_to(index) || !video.next(f)) return {};
    cv::Mat gray;
    cv::cvtColor(f.bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

// ── run ───────────────────────────────────────────────────────────────────────
bool SessionProcessor::run()
{
    if (!ok_) return false;

    const std::vector<Transition> transitions = classify_transitions();
    frame_count_ = static_cast<long>(transitions.size());
    if (frame_count_ == 0) return false;

    const SessionSegmenter::Result seg = SessionSegmenter{}.segment(transitions);

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

    // Assign every visit to a page (REVISIT merges, NAVIGATE starts a page).
    PageRegistry registry(video_path_, frame_w_, frame_h_, fps_, verbose_);
    navigations_.clear();
    int prev_page = 0;
    for (size_t i = 0; i < seg.visits.size(); ++i) {
        const PageVisit& visit = seg.visits[i];
        const cv::Mat first_gray = read_frame_gray(visit.firstFrame);
        if (first_gray.empty()) continue;

        const PageRegistry::Assignment a = registry.assign(visit, first_gray);
        if (i > 0)
            navigations_.push_back({visit.firstFrame, prev_page, a.pageId,
                                    a.navType, a.confidence, a.vtStart});
        prev_page = a.pageId;
    }

    // Collect the distinct pages.
    pages_.clear();
    for (const PageRegistry::PageRecord& rec : registry.records()) {
        PageResult page;
        page.image        = rec.canvas;
        page.docHeight    = rec.stitcher->doc_height();
        page.fixedRects   = rec.stitcher->fixed_rects();
        page.overlays     = rec.stitcher->overlays();
        page.scrollEvents = rec.stitcher->scroll_events();
        page.visits       = rec.stitcher->visits();
        pages_.push_back(std::move(page));
    }

    return !pages_.empty();
}
