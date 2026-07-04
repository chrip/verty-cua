#include "session_processor.hpp"
#include <iostream>

SessionProcessor::SessionProcessor(std::string video_path, bool verbose,
                                   Sensitivity sensitivity)
    : video_path_(std::move(video_path)), verbose_(verbose),
      sensitivity_(sensitivity), classifier_(sensitivity)
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
std::vector<Transition> SessionProcessor::classify_transitions(const cv::Mat& seed_noise)
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

    FixedAccumulator   chrome(frame_w_, frame_h_);
    DynamicAccumulator noise(frame_w_, frame_h_);
    noise.seed(seed_noise);                       // pass 2: ad already known
    cv::Mat chrome_mask, noise_mask = noise.mask();

    counts_ = {};
    while (video.next(f)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
        Transition tr = classifier_.classify(prev_gray, curr_gray,
                                             chrome_mask, noise_mask);
        counts_.add(tr.type);
        transitions.push_back(tr);

        switch (tr.type) {
        case TransitionType::Scroll:
            chrome.accumulate(prev_gray, curr_gray);
            chrome_mask = chrome.mask();
            break;
        case TransitionType::Identical:
        case TransitionType::Unstable:
            // Change without a scroll: evidence of persistent noise.
            noise.accumulate(prev_gray, curr_gray);
            noise_mask = noise.mask();
            break;
        case TransitionType::PageChange:
            // Chrome is per page — reset. The noise accumulator is NOT reset
            // and even ACCUMULATES here: a fixed video ad's churn is initially
            // classified PAGE_CHANGE (chicken-and-egg — the evidence would
            // otherwise never build up). A real navigation contributes only
            // 1–3 transitions, far below DYNAMIC_CONFIRM_FRAMES; an ad storm
            // contributes dozens and matures the mask.
            noise.accumulate(prev_gray, curr_gray);
            noise_mask = noise.mask();
            chrome.reset();
            chrome_mask = cv::Mat();
            break;
        }
        prev_gray = curr_gray.clone();
    }
    noise_mask_ = noise_mask;
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

    // Pass 1 classifies online and matures the persistent-noise mask (a fixed
    // video ad's churn is classified PAGE_CHANGE until the mask confirms it —
    // chicken-and-egg). Pass 2 re-classifies with the ad masked from frame
    // one, so the storm collapses to Identical and no longer fractures the
    // single page into spurious visits. Only pass 2 when there IS confirmed
    // noise — the common case does one pass.
    std::vector<Transition> transitions = classify_transitions();
    if (!noise_mask_.empty() && cv::countNonZero(noise_mask_) > 0) {
        if (verbose_)
            std::cout << "[session] persistent noise confirmed ("
                      << cv::countNonZero(noise_mask_) << "px) — reclassifying\n";
        transitions = classify_transitions(noise_mask_);
    }
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
    // Content-free visits — blank loading screens between navigations on real
    // recordings — are dropped: a page has content, a white frame does not.
    PageRegistry registry(video_path_, frame_w_, frame_h_, fps_, verbose_,
                          sensitivity_);
    navigations_.clear();
    int  prev_page = 0;
    bool have_page = false;
    for (const PageVisit& visit : seg.visits) {
        const cv::Mat first_gray = read_frame_gray(visit.firstFrame);
        if (first_gray.empty()) continue;

        cv::Scalar mean, stddev;
        cv::meanStdDev(first_gray, mean, stddev);
        if (stddev[0] < BLANK_VISIT_STDDEV) {
            if (verbose_)
                std::cout << "[session] visit [" << visit.firstFrame << ".."
                          << visit.lastFrame << "] dropped: blank (stddev "
                          << stddev[0] << ")\n";
            continue;
        }

        const PageRegistry::Assignment a = registry.assign(visit, first_gray);
        if (have_page)
            navigations_.push_back({visit.firstFrame, prev_page, a.pageId,
                                    a.navType, a.confidence, a.vtStart});
        prev_page = a.pageId;
        have_page = true;
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
