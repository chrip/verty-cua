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
std::vector<Transition> SessionProcessor::classify_transitions(
    const cv::Mat& seed_noise, const std::vector<cv::Rect>& subframe_rects)
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
                                             chrome_mask, noise_mask, subframe_rects);
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
        // Now that every state has its image, states that do not differ from the
        // one before can be recognised and dropped.
        subframes.drop_indistinct_states();
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
    // Pass 2 re-classifies with what pass 1 learned: where the persistent noise
    // is, and which rectangles change in place while the document stands still. A
    // scroll dy contained in one of those is that region's, not the document's —
    // and pass 1 could not know it, because the regions are only grouped once
    // every change has been seen.
    std::vector<cv::Rect> subframe_rects;
    for (const auto& sf : subframes_) subframe_rects.push_back(sf.rect);
    const bool have_noise = !noise_mask_.empty() && cv::countNonZero(noise_mask_) > 0;
    if (have_noise || !subframe_rects.empty()) {
        if (verbose_) {
            std::cout << "[session] reclassifying with";
            if (have_noise)
                std::cout << " persistent noise (" << cv::countNonZero(noise_mask_) << "px)";
            if (have_noise && !subframe_rects.empty()) std::cout << " and";
            if (!subframe_rects.empty())
                std::cout << " " << subframe_rects.size() << " in-place region(s)";
            std::cout << "\n";
        }
        transitions = classify_transitions(have_noise ? noise_mask_ : cv::Mat(), subframe_rects);
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
    registry_ = std::make_unique<PageRegistry>(video_path_, frame_w_, frame_h_, fps_,
                                               verbose_, sensitivity_, scrollbar_.get());
    PageRegistry& registry = *registry_;
    // The stitchers run their own classifier passes, so they need what pass 1
    // learned about in-place regions too.
    registry.set_subframes(subframe_rects);
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
        // The one document offset every frame in `evidence` agrees on, or -1 if
        // they do not agree or none of them carries an offset. Frames with no
        // offset (a page-change window, a blank visit) are skipped rather than
        // counted as disagreement: they make no claim to disagree with.
        //
        // Exact agreement, not a tolerance. The offset is published as the
        // document row of a viewport rect, i.e. as the place to lay this region's
        // images over the page stitch, and a row averaged out of offsets that
        // differ is a guess. Measured offset spread over the evidence frames of all
        // 16 reported page-slices in the corpus: exactly 0px on 14 of them — one of
        // them a noise region whose evidence is 627 localized change frames —
        // against 660px (rw1_newsportal's sticky right rail, 481×956 at the frame
        // edge, changing over an offset range of 660) and 2999px (an in-content ad
        // that reloaded at two offsets 3000px apart) for the two that must name no
        // row at all. So exactness costs nothing measured, and the gap it sits in
        // is three orders of magnitude wide — there is no tolerance to calibrate.
        auto agreed_vt = [&](const std::vector<long>& evidence) -> long {
            long v = -1;
            for (long f : evidence) {
                if (f < 0 || f >= frame_count_) continue;
                const long u = frames_[static_cast<size_t>(f)].vt;
                if (u < 0) continue;
                if (v >= 0 && u != v) return -1;
                v = u;
            }
            return v;
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
            for (auto& [p, ps] : per_page) {
                if (p < 0 || static_cast<size_t>(p) >= pages_.size()) continue;
                // Was the region actually anchored to the viewport while these
                // states happened? Its rect is a viewport rect, so if the document
                // moved between them, the rect does not name one region and the
                // crops are strips of whatever was at those coordinates.
                long lo = -1, hi = -1;
                for (const auto& st : ps.states) {
                    if (st.frame < 0 || st.frame >= frame_count_) continue;
                    const long v = frames_[static_cast<size_t>(st.frame)].vt;
                    if (v < 0) continue;
                    if (lo < 0 || v < lo) lo = v;
                    if (hi < 0 || v > hi) hi = v;
                }
                if (lo >= 0 && hi - lo > SUBFRAME_MAX_ANCHOR_DRIFT) continue;

                // Where the region is in DOCUMENT coordinates on this page. Only
                // rect.y needs it: rect.x, w and h are the same numbers in both
                // spaces, because nothing in this tool models horizontal scrolling
                // (ScrollEvent has only dy, frames[] only vt).
                //
                // A slice WITH states is evidenced by their frames, because the
                // states are what the report hands over to be placed: each state
                // image is a crop of `rect`, and the page canvas is stamped with one
                // of them, so anchoring to the states makes the published row equal
                // the row the stamp was painted at, by construction (verified: the
                // stamping log below prints document row 709 for session_gallery,
                // and 709 is what the report states). Measured on rw6_shopcatalog
                // page 3, why the two evidence sets are not interchangeable: its
                // states agree on 960 while its change frames span 958..960, so
                // change frames would refuse a row that the state images have.
                //
                // A slice WITHOUT states — a region classified as dynamic noise —
                // has no state frame at all, and its change frames are the only
                // evidence there is. Measured: without this branch none of the six
                // noise slices in the corpus can be placed, though four of them are
                // ordinary in-flow elements (the `position: relative` panels of the
                // gallery and hostile fixtures, a carousel in rw6_shopcatalog) that
                // do sit at one document row — 711 for the gallery video panel,
                // agreed by all 627 of its localized change frames.
                //
                // What agreement proves, and what it does not. It proves the
                // document stood still across every frame this region is evidenced
                // by, so the row is where the stitch wrote those pixels. It does NOT
                // prove the region moves WITH the document: a viewport-fixed element
                // that only churns while the page is still would agree too, and a
                // single change frame agrees with itself. Asked of the pixels
                // directly — compare the region's rect between two frames at
                // different offsets, once unshifted and once shifted by the offset
                // difference — the four in-flow noise regions above answer clearly
                // (unshifted 69.7/83.0/93.6/119.1 MAD against shifted
                // 39.8/35.2/35.6/0.7), so the branch is right where it is used. But
                // the same question put to the screen flows does not separate: a
                // product grid is periodic enough that rw6's page 6 prefers
                // "viewport" (28.9 vs 37.5) for the region its page 0 shows in flow
                // at 170:1, and an offset difference of 2px decides nothing. So the
                // anchor is deliberately NOT reported as a verdict — this field
                // states a row and what founds it, and nothing about CSS.
                std::vector<long> evidence;
                if (!ps.states.empty()) {
                    for (const auto& st : ps.states) evidence.push_back(st.frame);
                } else {
                    for (long f : sf.change_frames)
                        if (page_of(f) == p) evidence.push_back(f);
                }
                const long vt = agreed_vt(evidence);
                if (vt >= 0) ps.document_y = ps.rect.y + vt;

                // Each state also gets its OWN row, from its own frame. The
                // region-level row above is a summary that exists only while every
                // observation agrees; a state's row is a fact about one frame and
                // exists whenever that frame was localized. Without the per-state
                // row there is a band of regions — states spread by 1 to
                // SUBFRAME_MAX_ANCHOR_DRIFT px, which passes the veto above — that
                // is reported with no way to place any of its images, though every
                // one of them has an exact place. (The comment on that constant
                // cites a real 68px embedded-phone case, so the band is reachable;
                // no session in the corpus is in it today.)
                for (auto& st : ps.states) {
                    if (st.frame < 0 || st.frame >= frame_count_) continue;
                    const long v = frames_[static_cast<size_t>(st.frame)].vt;
                    if (v >= 0) st.document_y = ps.rect.y + v;
                }

                pages_[static_cast<size_t>(p)].subframes.push_back(std::move(ps));
            }
        }
        for (PageResult& pg : pages_)
            std::sort(pg.subframes.begin(), pg.subframes.end(),
                      [](const PageSubframe& a, const PageSubframe& b) {
                          if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
                          return a.rect.x < b.rect.x;
                      });

        // ── One settled appearance per in-place region, in the page image ─────
        // The canvas has nothing true to show inside such a region: the per-row
        // vote elects whichever appearance a row's frames agreed on, and with
        // several mutually exclusive appearances and no majority, adjacent rows
        // elect different ones — a superposition of screens under otherwise
        // correct chrome. Stamping one settled state replaces that with an instant
        // the region actually had. Every state stays in the report with its own
        // image, so nothing is lost, only chosen.
        //
        // The last settled state is the one chosen, because an independent ground
        // truth says so: the browser's own full-page reference for the tapped-panel
        // fixture is captured after the interactions, and its panel is
        // pixel-identical to that region's last settled state (the fixture cycles
        // three appearances over 30 taps, and the reference holds the one tap 30
        // left behind). A page screenshot is the page as the session left it, and
        // that is the appearance it agrees with.
        for (size_t pi = 0; pi < pages_.size() && pi < registry_->records().size(); ++pi) {
            PageResult& page = pages_[pi];
            // A state can only be placed if this page knows where the viewport was
            // on its frame — its rectangle is in viewport coordinates and the canvas
            // is in document coordinates. So the state chosen is the last one whose
            // frame this page actually held; a page image missing a region is
            // honest, one showing it at a guessed document row is not.
            auto offset_of = [&page](long frame, long& vt) {
                for (const PageStitcher::VisitTrace& t : page.visits) {
                    const long local = frame - t.visit.firstFrame;
                    if (local < 0 || local >= static_cast<long>(t.vt_by_frame.size()))
                        continue;
                    vt = t.vt_by_frame[static_cast<size_t>(local)];
                    return true;
                }
                return false;
            };
            bool stamped = false;
            for (PageSubframe& sf : page.subframes) {
                if (!sf.screenflow) continue;
                for (size_t si = sf.states.size(); si-- > 0;) {
                    const auto& st = sf.states[si];
                    long vt = 0;
                    if (st.image.empty() || !offset_of(st.frame, vt)) continue;
                    registry_->records()[pi].stitcher->add_stamp(sf.rect, st.image, st.frame);
                    sf.stamped_state = static_cast<int>(si);
                    stamped = true;
                    if (verbose_)
                        std::cout << "[page " << pi << "] stamping its in-place region "
                                  << sf.rect.width << "×" << sf.rect.height << " at ("
                                  << sf.rect.x << "," << sf.rect.y << ") with state "
                                  << si << " of " << sf.states.size() << " (frame "
                                  << st.frame << ", document row "
                                  << sf.rect.y + vt << ")\n";
                    break;
                }
            }
            if (stamped)
                page.image = registry_->records()[pi].stitcher->reconstruct();
        }
    }

    return !pages_.empty();
}
