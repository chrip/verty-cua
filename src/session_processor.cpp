#include "session_processor.hpp"
#include "subframe_stitcher.hpp"
#include <algorithm>
#include <iostream>
#include <map>

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
    if (ok_) scrollbar_ = std::make_unique<ScrollbarTracker>(frame_w_, frame_h_);
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
    if (scrollbar_) scrollbar_->observe(curr_gray);

    FixedAccumulator   chrome(frame_w_, frame_h_);
    DynamicAccumulator noise(frame_w_, frame_h_);
    // Subframes are measured on the FIRST pass only: the second pass exists to
    // re-classify with the noise mask seeded, and the question this tracker asks
    // — did a bounded region hold still after changing — is about the recording,
    // not about how the classifier reads it.
    const bool track_subframes = seed_noise.empty();
    SubframeTracker subframes(frame_w_, frame_h_, fps_, verbose_);
    noise.seed(seed_noise);                       // pass 2: ad already known
    cv::Mat chrome_mask, noise_mask = noise.mask();

    counts_ = {};
    while (video.next(f)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
        // The scrollbar band is a property of the session, so it is learned in
        // the pass that already visits every frame. observe() no-ops once the
        // band is located, so a second classification pass costs nothing.
        if (scrollbar_) scrollbar_->observe(curr_gray);
        Transition tr = classifier_.classify(prev_gray, curr_gray,
                                             chrome_mask, noise_mask);
        counts_.add(tr.type);
        transitions.push_back(tr);
        if (track_subframes)
            subframes.observe(f.index, prev_gray, curr_gray,
                              tr.type == TransitionType::Scroll);

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
            noise.accumulate(prev_gray, curr_gray);
            noise_mask = noise.mask();
            // The chrome estimate is NOT discarded here. Feeding the
            // page-change transition to it like any other transition is
            // strictly more informative: pixels that changed reset to 0 (chrome
            // that really did change is cleared), pixels that did not keep
            // counting (a shared template, or a host page whose embedded
            // subframe is what changed). Blanket-resetting threw the second
            // group away, so on a session whose page changes are in-place
            // mutations the estimate never matured at all — measured on an
            // embedded-prototype recording: 1529752 px of confirmed chrome
            // immediately before a page change, 0 for the whole visit after,
            // which left the classifier normalising against the whole frame for
            // the rest of the session.
            chrome.accumulate(prev_gray, curr_gray);
            chrome_mask = chrome.mask();
            break;
        }
        prev_gray = curr_gray.clone();
    }
    noise_mask_ = noise_mask;
    if (track_subframes) {
        subframes.finish();
        // One more sequential walk, and only when there is a screen flow to
        // photograph: its states are single frames scattered through the video, so
        // seeking to each would decode most of it several times over.
        const std::vector<long> want = subframes.state_frames_needed();
        if (!want.empty()) {
            VideoSource src(video_path_);
            VideoSource::Frame g;
            size_t next = 0;
            while (next < want.size() && src.next(g)) {
                while (next < want.size() && want[next] < g.index) ++next;
                if (next < want.size() && want[next] == g.index) {
                    subframes.set_state_image(g.index, g.bgr);
                    ++next;
                }
            }
        }
        subframes_ = subframes.subframes();
    }
    if (scrollbar_) scrollbar_->locate();
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
        if (scrollbar_ && scrollbar_->located()) {
            const cv::Rect b = scrollbar_->band();
            std::cout << "[session] vertical scrollbar at x=" << b.x << ".."
                      << b.x + b.width - 1 << " (" << b.width << "px wide)\n";
        } else {
            std::cout << "[session] no vertical scrollbar found\n";
        }
        std::cout << "[session] visits=" << seg.visits.size();
        for (const PageVisit& v : seg.visits)
            std::cout << "  [" << v.firstFrame << ".." << v.lastFrame << "]";
        std::cout << "\n";
    }

    // Assign every visit to a page (REVISIT merges, NAVIGATE starts a page).
    // Content-free visits — blank loading screens between navigations on real
    // recordings — are dropped: a page has content, a white frame does not.
    PageRegistry registry(video_path_, frame_w_, frame_h_, fps_, verbose_,
                          sensitivity_, scrollbar_.get());
    navigations_.clear();
    std::vector<PageVisit> blank_visits;
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
            blank_visits.push_back(visit);
            continue;
        }

        const PageRegistry::Assignment a = registry.assign(visit, first_gray);
        // A continuation is NOT a navigation: the viewport jumped inside one
        // document. The jump is reported as a large scroll event by the page's
        // stitcher, so emitting a navigation here would double-count it and
        // claim a page transition that never happened.
        if (have_page && !a.continuation)
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
        page.capturedExtent = rec.stitcher->captured_extent();
        page.documentHeight = rec.stitcher->document_height_estimate();
        page.documentHeightUncertainty = rec.stitcher->document_height_uncertainty();
        page.driftPx        = rec.stitcher->drift_px();
        page.reachedBottom  = rec.stitcher->reached_bottom();
        page.fixedRects   = rec.stitcher->fixed_rects();
        page.overlays     = rec.stitcher->overlays();
        page.scrollEvents = rec.stitcher->scroll_events();
        page.visits       = rec.stitcher->visits();
        page.gaps         = rec.stitcher->gaps();
        pages_.push_back(std::move(page));
    }

    // ── Per-frame answer table ───────────────────────────────────────────────
    // Every frame of the video gets a row. Frames the segmenter left outside any
    // visit (its page-change debounce window) and frames of a visit dropped as
    // blank carry no offset, and say which of the two they are, rather than
    // simply being absent from the report.
    frames_.assign(static_cast<size_t>(frame_count_), {});
    for (long i = 0; i < frame_count_; ++i) {
        frames_[static_cast<size_t>(i)].frame  = i;
        frames_[static_cast<size_t>(i)].t      = static_cast<double>(i) / fps_;
        frames_[static_cast<size_t>(i)].reason = "transition";
    }
    for (const PageVisit& v : blank_visits)
        for (long i = v.firstFrame; i <= v.lastFrame && i < frame_count_; ++i)
            frames_[static_cast<size_t>(i)].reason = "blank";
    for (size_t pi = 0; pi < pages_.size(); ++pi)
        for (const PageStitcher::VisitTrace& t : pages_[pi].visits)
            for (size_t k = 0; k < t.vt_by_frame.size(); ++k) {
                const long i = t.visit.firstFrame + static_cast<long>(k);
                if (i < 0 || i >= frame_count_) continue;
                FrameRecord& f = frames_[static_cast<size_t>(i)];
                f.pageId = static_cast<int>(pi);
                f.vt     = t.vt_by_frame[k];
                f.reason.clear();
            }

    // ── Recursion into subframes ─────────────────────────────────────────────
    // A subframe that scrolled its own content is a document, so the same
    // classifier and the same stitcher run on its rectangle (see
    // subframe_stitcher.hpp). Only screen flows are worth the passes: a region
    // classified as noise has no document to recover.
    //
    // It runs HERE, after the per-frame offsets exist, because the inner runs have
    // to be bounded by the DOCUMENT's own movement — a rectangle fixed in frame
    // coordinates looks at page content sliding past while the page scrolls, and
    // the inner classifier cannot tell that from the subframe scrolling itself.
    // The classifier's own Scroll verdicts are not a reliable substitute:
    // measured on the auto-refreshing news portal, where its ad churn leaves the
    // session-level stream with 4 Scroll verdicts for ~25 real scrolls, using them
    // produced a 4085px "inner document" that was simply the article scrolling.
    if (!subframes_.empty()) {
        std::vector<bool> doc_moved(static_cast<size_t>(frame_count_), false);
        for (long i = 1; i < frame_count_; ++i) {
            const auto& a = frames_[static_cast<size_t>(i - 1)];
            const auto& b = frames_[static_cast<size_t>(i)];
            // vt = -1 means the frame carries no offset (a page-change window or
            // a blank visit): unknown counts as moved, because a run may not span
            // a gap in the document's own trace.
            doc_moved[static_cast<size_t>(i)] = a.vt < 0 || b.vt < 0 || a.vt != b.vt;
        }
        for (size_t i = 0; i < subframes_.size(); ++i) {
            if (!subframes_[i].is_screenflow()) continue;
            SubframeStitcher inner(video_path_, fps_, verbose_);
            for (auto& d : inner.run(subframes_[i].rect, 0, frame_count_ - 1,
                                     static_cast<int>(i), doc_moved)) {
                SubframeTracker::InnerDoc id;
                id.first_frame = d.first_frame;
                id.last_frame  = d.last_frame;
                id.extent      = d.extent;
                id.scrolls     = d.scrolls;
                id.image       = std::move(d.image);
                subframes_[i].inner.push_back(std::move(id));
            }
        }

        // ── Attribute each region to the page(s) it happened on ──────────────
        // Detection is session-wide, because a region is one region whether or
        // not the session navigated while it was there. Reporting is per page,
        // because that is where a consumer looks for it — and it is what lets the
        // written files carry their page in their name.
        auto page_of = [&](long frame) {
            if (frame < 0 || frame >= frame_count_) return -1;
            return frames_[static_cast<size_t>(frame)].pageId;
        };
        for (const auto& sf : subframes_) {
            std::map<int, PageSubframe> per_page;
            auto slot = [&](int page) -> PageSubframe& {
                auto it = per_page.find(page);
                if (it == per_page.end()) {
                    PageSubframe ps;
                    ps.rect       = sf.rect;
                    ps.screenflow = sf.is_screenflow();
                    it = per_page.emplace(page, std::move(ps)).first;
                }
                return it->second;
            };
            for (long f : sf.change_frames) {
                const int p = page_of(f);
                if (p >= 0) ++slot(p).in_place_changes;
            }
            for (const auto& st : sf.states) {
                const int p = page_of(st.frame);
                if (p >= 0) slot(p).states.push_back(st);
            }
            for (const auto& d : sf.inner) {
                const int p = page_of(d.first_frame);
                if (p >= 0) slot(p).inner.push_back(d);
            }
            for (auto& [p, ps] : per_page)
                if (p >= 0 && static_cast<size_t>(p) < pages_.size())
                    pages_[static_cast<size_t>(p)].subframes.push_back(std::move(ps));
        }
        for (PageResult& pg : pages_)
            std::sort(pg.subframes.begin(), pg.subframes.end(),
                      [](const PageSubframe& a, const PageSubframe& b) {
                          if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
                          return a.rect.x < b.rect.x;
                      });
    }

    return !pages_.empty();
}
