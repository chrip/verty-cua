#include "page_stitcher.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

// ── construction ──────────────────────────────────────────────────────────────
PageStitcher::PageStitcher(std::string video_path, int frame_w, int frame_h,
                           double fps, bool verbose, int page_id,
                           const ScrollbarTracker* scrollbar)
    : video_path_(std::move(video_path)),
      frame_w_(frame_w), frame_h_(frame_h), fps_(fps),
      verbose_(verbose), page_id_(page_id),
      accumulator_(frame_w, frame_h),
      noise_accumulator_(frame_w, frame_h),
      scrollbar_(scrollbar),
      sb_map_(frame_h)
{
    fixed_mask_ = cv::Mat::zeros(frame_h_, frame_w_, CV_8UC1);

    // Everything from the located band to the right edge is scrollbar: the one
    // or two columns the band stops short of are its own anti-aliased outline
    // (SB_EDGE_SLACK), not page content.
    if (scrollbar_ && scrollbar_->located()) {
        const int w = frame_w_ - scrollbar_->band().x;
        if (w > 0 && w <= SB_MAX_WIDTH + SB_EDGE_SLACK) sb_w_ = w;
    }
}

VideoSource PageStitcher::open_at(const PageVisit& v) const
{
    VideoSource video(video_path_);
    if (video.ok()) video.skip_to(v.firstFrame);
    return video;
}

// ── add_visit ─────────────────────────────────────────────────────────────────
void PageStitcher::add_visit(const PageVisit& visit, long vt_start,
                             bool bridged, long vt_uncertainty)
{
    if (frame_w_ <= 0 || frame_h_ <= 0) return;

    // A re-localized jump is a SCROLL, not a navigation: the viewport moved
    // within one document. Emitting it as a scroll event keeps the contract's
    // first deliverable truthful — the trace would otherwise show the document
    // teleporting between two visits with no event to account for it.
    if (bridged && !visits_.empty() && !visits_.back().vt_by_frame.empty()) {
        const long from = visits_.back().vt_by_frame.back();
        scroll_events_.push_back({visit.firstFrame,
                                  static_cast<double>(visit.firstFrame) / fps_,
                                  static_cast<int>(vt_start - from),
                                  vt_start});
    }

    analyze_visit(visit, vt_start);
    refine_visit(visit, vt_start, bridged, vt_uncertainty);
    // After refine: needs the band-source frames for its texture filter.
    extract_overlays();
}

// ── analyze_visit (PASS 1) ────────────────────────────────────────────────────
// Accumulate the fixed-pixel mask (shared across the page's visits) and a
// rough per-frame scroll offset for this visit.
void PageStitcher::analyze_visit(const PageVisit& visit, long vt_start)
{
    VideoSource video = open_at(visit);
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    if (!video.next(f) || f.index > visit.lastFrame) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();

    long vt = std::max(0L, vt_start);
    while (video.next(f) && f.index <= visit.lastFrame) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray,
                                             cv::Mat(), noise_accumulator_.mask());
        if (tr.type == TransitionType::Scroll) {
            accumulator_.accumulate(prev_gray, curr_gray);
            vt = std::max(0L, vt + tr.dy);
        } else {
            noise_accumulator_.accumulate(prev_gray, curr_gray);
        }
        prev_gray = curr_gray.clone();
    }

    fixed_mask_ = accumulator_.mask();
    noise_mask_ = noise_accumulator_.mask();
    extract_rects();

    if (verbose_)
        std::cout << "[page " << page_id_ << " analyze] visit ["
                  << visit.firstFrame << ".." << visit.lastFrame << "]"
                  << "  fixed header: " << header_h_ << "px"
                  << "  footer: " << footer_h_ << "px"
                  << "  left: "   << left_w_   << "px"
                  << "  right: "  << right_w_  << "px\n";
}

// ── refine_visit (PASS 2) ─────────────────────────────────────────────────────
// Re-classify the visit's transitions with the page's fixed mask, producing the
// authoritative scroll offsets. Also tracks the frames showing the document's
// shallowest / deepest point (band sources for stamping).
void PageStitcher::refine_visit(const PageVisit& visit, long vt_start,
                                bool bridged, long vt_uncertainty)
{
    VideoSource video = open_at(visit);
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    if (!video.next(f) || f.index > visit.lastFrame) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();

    VisitTrace trace;
    trace.visit          = visit;
    trace.vt_start       = vt_start;
    trace.vt_uncertainty = vt_uncertainty;
    trace.bridged        = bridged;

    long vt = std::max(0L, vt_start);
    trace.vt_by_frame.push_back(vt);

    auto track_extremes = [&](long v, const cv::Mat& bgr) {
        if (min_vt_ < 0 || v <= min_vt_) { min_vt_ = v; bgr.copyTo(first_frame_); }
        if (v >= max_vt_)                { max_vt_ = v; bgr.copyTo(last_frame_);  }
    };
    track_extremes(vt, f.bgr);

    // Calibrate this page's scrollbar map from the offsets tracked here. The
    // offsets are authoritative (this is the mask-aware pass), so they are the
    // right anchors; the map refuses anchors that contradict it, which keeps it
    // tied to the trustworthy prefix if the chain later breaks.
    auto calibrate = [&](const cv::Mat& gray, long v) {
        if (scrollbar_ && scrollbar_->located()) sb_map_.add(scrollbar_->read(gray), v);
    };
    // Thumb-free scrollbar strip, accumulated as the per-pixel MAXIMUM over
    // every frame of every visit: the thumb is darker than its track and moves
    // on, so the maximum at any row is the track, while the arrow glyphs never
    // move and survive. Used to paint the column, which is excluded from the
    // stitched interior (see stamp_scrollbar).
    auto accumulate_bar = [&](const cv::Mat& bgr) {
        if (sb_w_ <= 0) return;
        const cv::Mat strip = bgr(cv::Rect(frame_w_ - sb_w_, 0, sb_w_, frame_h_));
        if (sb_clean_.empty()) strip.copyTo(sb_clean_);
        else                   cv::max(sb_clean_, strip, sb_clean_);
    };
    calibrate(curr_gray, vt);
    accumulate_bar(f.bgr);

    while (video.next(f) && f.index <= visit.lastFrame) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray,
                                             fixed_mask_, noise_mask_);
        if (tr.type == TransitionType::Scroll)
            vt = std::max(0L, vt + tr.dy);

        trace.vt_by_frame.push_back(vt);
        track_extremes(vt, f.bgr);
        calibrate(curr_gray, vt);
        accumulate_bar(f.bgr);
        prev_gray = curr_gray.clone();
    }

    append_scroll_events(trace);
    visits_.push_back(std::move(trace));

    if (verbose_) {
        std::cout << "[page " << page_id_ << " refine] visit ["
                  << visit.firstFrame << ".." << visit.lastFrame << "]"
                  << "  vtStart=" << vt_start;
        if (vt_uncertainty > 0) std::cout << "±" << vt_uncertainty;
        std::cout << (bridged ? " (bridged)" : "")
                  << "  docHeight=" << doc_height() << "px"
                  << "  scrollEvents=" << scroll_events_.size() << "\n";
        if (sb_map_.valid())
            std::cout << "[page " << page_id_ << " scrollbar] thumbH="
                      << sb_map_.fingerprint() << "  map vt="
                      << sb_map_.slope() << "*thumbTop+b  ("
                      << (sb_map_.slope_fitted() ? "fitted" : "geometric")
                      << ", " << sb_map_.points() << " anchors, residual "
                      << sb_map_.residual() << "px, " << sb_map_.rejected()
                      << " rejected)\n";
    }
}

// ── append_scroll_events ──────────────────────────────────────────────────────
// Every frame whose vt differs from its predecessor is one scroll step.
// Frame indices and timestamps are global (session coordinates).
void PageStitcher::append_scroll_events(const VisitTrace& trace)
{
    for (size_t i = 1; i < trace.vt_by_frame.size(); ++i) {
        long dy = trace.vt_by_frame[i] - trace.vt_by_frame[i - 1];
        if (dy == 0) continue;
        const long frame = trace.visit.firstFrame + static_cast<long>(i);
        scroll_events_.push_back({
            frame,
            static_cast<double>(frame) / fps_,
            static_cast<int>(dy),
            trace.vt_by_frame[i],
        });
    }
}

// ── extract_rects ─────────────────────────────────────────────────────────────
// Reduce the fixed mask to the four edge-anchored bands. Each band is the run of
// contiguous fully-fixed rows/columns from an edge, so its extent is pixel exact.
// Only edge-anchored chrome is handled: interior "floating" fixed elements are
// not extracted, because frame-difference yields false positives on locally
// static content. Reconstruction copies the scroll interior wholesale, so those
// false positives simply remain part of the (correct) scroll content.
void PageStitcher::extract_rects()
{
    rects_.clear();
    const cv::Mat& m = fixed_mask_;
    const int W = frame_w_, H = frame_h_;

    auto row_all_fixed = [&](int y) {
        const uint8_t* row = m.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) if (row[x] != 255) return false;
        return true;
    };
    auto col_all_fixed = [&](int x) {
        for (int y = 0; y < H; ++y) if (m.at<uint8_t>(y, x) != 255) return false;
        return true;
    };

    header_h_ = 0; for (int y = 0; y < H; ++y)              { if (row_all_fixed(y)) ++header_h_; else break; }
    footer_h_ = 0; for (int y = H - 1; y >= header_h_; --y) { if (row_all_fixed(y)) ++footer_h_; else break; }
    left_w_   = 0; for (int x = 0; x < W; ++x)              { if (col_all_fixed(x)) ++left_w_;   else break; }
    right_w_  = 0; for (int x = W - 1; x >= left_w_; --x)   { if (col_all_fixed(x)) ++right_w_;  else break; }

    // The browser's own scrollbar. Note that a genuine fixed right SIDEBAR can
    // no longer be found when a scrollbar is present: right_w_ counts contiguous
    // fully-fixed columns inward from the edge, and the bar's moving thumb stops
    // that scan at the first column. Left as an observation rather than fixed —
    // no session in the corpus has both, so there is nothing to measure a fix
    // against (tests-before-algorithms).
    if (sb_w_ > 0)
        rects_.push_back({ cv::Rect(W - sb_w_, 0, sb_w_, H), FixedKind::Scrollbar });

    const int mid_h = H - header_h_ - footer_h_;
    if (header_h_ > 0)             rects_.push_back({ cv::Rect(0, 0, W, header_h_),                     FixedKind::Header });
    if (footer_h_ > 0)             rects_.push_back({ cv::Rect(0, H - footer_h_, W, footer_h_),         FixedKind::Footer });
    if (left_w_  > 0 && mid_h > 0) rects_.push_back({ cv::Rect(0, header_h_, left_w_, mid_h),             FixedKind::Left  });
    if (right_w_ > 0 && mid_h > 0) rects_.push_back({ cv::Rect(W - right_w_, header_h_, right_w_, mid_h), FixedKind::Right });
}

// ── extract_overlays ──────────────────────────────────────────────────────────
// Interior fixed blobs = viewport-fixed elements that are not edge-anchored
// (floating buttons, cookie banners). Filters separate genuine overlays from
// stable whitespace, the historical false positive: overlays are COMPACT
// (fixed-pixel fill of the bbox) and TEXTURED (whitespace is uniform).
// Merge one verified overlay into the page-wide list. Overlays are collected
// across ALL visits of the page (not just the last): a revisit merges several
// visits, and a floating element verified in one visit must stay masked for
// every visit's interior copy — otherwise it is baked into the scroll content
// at each position it was seen. Rects covering the same element (>50% of the
// smaller overlapping the other) are unioned to their bounding box.
void PageStitcher::add_overlay(const cv::Rect& o)
{
    for (cv::Rect& e : overlays_) {
        const cv::Rect inter = e & o;
        const int smaller = std::min(e.area(), o.area());
        if (smaller > 0 && inter.area() > smaller / 2) { e |= o; return; }
    }
    overlays_.push_back(o);
}

void PageStitcher::extract_overlays()
{
    if (last_frame_.empty() && first_frame_.empty()) return;

    cv::Mat rem = fixed_mask_.clone();
    const int W = frame_w_, H = frame_h_;
    if (header_h_ > 0) rem(cv::Rect(0, 0, W, header_h_)).setTo(0);
    if (footer_h_ > 0) rem(cv::Rect(0, H - footer_h_, W, footer_h_)).setTo(0);
    if (left_w_  > 0)  rem(cv::Rect(0, 0, left_w_, H)).setTo(0);
    if (right_w_ > 0)  rem(cv::Rect(W - right_w_, 0, right_w_, H)).setTo(0);
    if (sb_w_    > 0)  rem(cv::Rect(W - sb_w_, 0, sb_w_, H)).setTo(0);   // chrome

    cv::morphologyEx(rem, rem, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

    // Stable whitespace is fixed too and CONNECTS to any overlay it touches,
    // sprawling the component until the compactness test fails. Seed the
    // components from TEXTURED fixed pixels only (whitespace has no texture),
    // then measure compactness against the full fixed mask.
    const cv::Mat& sample = last_frame_.empty() ? first_frame_ : last_frame_;
    cv::Mat gray, lap, textured, seed;
    cv::cvtColor(sample, gray, cv::COLOR_BGR2GRAY);
    cv::Laplacian(gray, lap, CV_16S, 3);
    cv::convertScaleAbs(lap, lap);
    cv::compare(lap, 12, textured, cv::CMP_GT);
    cv::dilate(textured, textured,
               cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)),
               cv::Point(-1, -1), 2);
    cv::bitwise_and(rem, textured, seed);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(seed, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // An overlay may reach into an over-detected band (its margin is stable
    // whitespace, blanked above), which would crop the stamped element. Grow
    // each candidate on the UNBLANKED evidence — fixed AND textured pixels —
    // until the evidence stops (capped; the two-frame verification still
    // guards the final rect).
    cv::Mat evidence;
    cv::bitwise_and(fixed_mask_, textured, evidence);
    auto grow = [&](cv::Rect b) {
        constexpr int CAP = 32;
        auto col = [&](int x) { return cv::countNonZero(evidence(cv::Rect(x, b.y, 1, b.height))); };
        auto row = [&](int y) { return cv::countNonZero(evidence(cv::Rect(b.x, y, b.width, 1))); };
        for (int i = 0; i < CAP && b.x + b.width  < W && col(b.x + b.width) >= std::max(3, b.height / 5); ++i) ++b.width;
        for (int i = 0; i < CAP && b.x > 0            && col(b.x - 1)       >= std::max(3, b.height / 5); ++i) { --b.x; ++b.width; }
        for (int i = 0; i < CAP && b.y + b.height < H && row(b.y + b.height) >= std::max(3, b.width / 5); ++i) ++b.height;
        for (int i = 0; i < CAP && b.y > 0            && row(b.y - 1)       >= std::max(3, b.width / 5); ++i) { --b.y; ++b.height; }
        return b;
    };

    std::vector<cv::Rect> candidates;
    for (const auto& c : contours) {
        cv::Rect b = cv::boundingRect(c);
        if (cv::countNonZero(rem(b)) < OVERLAY_MIN_AREA) continue;
        const double fill = cv::countNonZero(rem(b)) / static_cast<double>(b.area());
        if (fill < OVERLAY_MIN_FILL) continue;

        cv::Scalar mean, stddev;
        cv::meanStdDev(gray(b), mean, stddev);
        if (stddev[0] < OVERLAY_MIN_STDDEV) continue;   // uniform: not UI

        b = grow(b);
        b.x = std::max(0, b.x - OVERLAY_PAD);
        b.y = std::max(0, b.y - OVERLAY_PAD);
        b.width  = std::min(W - b.x, b.width  + 2 * OVERLAY_PAD);
        b.height = std::min(H - b.y, b.height + 2 * OVERLAY_PAD);
        candidates.push_back(b);
    }
    // Persistent-noise region (a fixed video ad): confirmed by its own
    // accumulation — no two-frame verification (its content CHANGES; that is
    // the point). The ad's changing content splits into several disjoint
    // textured blobs across the session; they are ONE fixed element, so union
    // them into a single bounding box — otherwise each blob would be stamped
    // separately and the ad would appear several times down the page.
    if (!noise_mask_.empty()) {
        cv::Mat nm = noise_mask_.clone();
        if (header_h_ > 0) nm(cv::Rect(0, 0, W, header_h_)).setTo(0);
        if (footer_h_ > 0) nm(cv::Rect(0, H - footer_h_, W, footer_h_)).setTo(0);
        if (left_w_  > 0)  nm(cv::Rect(0, 0, left_w_, H)).setTo(0);
        if (right_w_ > 0)  nm(cv::Rect(W - right_w_, 0, right_w_, H)).setTo(0);
        if (sb_w_    > 0)  nm(cv::Rect(W - sb_w_, 0, sb_w_, H)).setTo(0);
        cv::morphologyEx(nm, nm, cv::MORPH_OPEN,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        if (cv::countNonZero(nm) >= OVERLAY_MIN_AREA) {
            cv::Rect b = cv::boundingRect(nm);   // union of all noise pixels
            b.x = std::max(0, b.x - OVERLAY_PAD);
            b.y = std::max(0, b.y - OVERLAY_PAD);
            b.width  = std::min(W - b.x, b.width  + 2 * OVERLAY_PAD);
            b.height = std::min(H - b.y, b.height + 2 * OVERLAY_PAD);
            add_overlay(b);
            if (verbose_)
                std::cout << "[page " << page_id_ << " noise-overlay] "
                          << b.width << "×" << b.height << " at viewport ("
                          << b.x << "," << b.y << ")\n";
        }
    }

    if (candidates.empty()) return;

    // Verification: a genuine viewport-fixed overlay is IDENTICAL in two
    // frames whose scroll positions are far apart — different document content
    // passed underneath it. Slowly-varying scrolled content (e.g. low-contrast
    // periodic textures that survive the per-transition stability test) is not.
    if (max_vt_ < 200) return;   // page barely scrolled: cannot verify → none
    long frameA = -1, frameB = -1, bestA = LONG_MAX, bestB = LONG_MAX;
    for (const VisitTrace& t : visits_) {
        for (size_t i = 0; i < t.vt_by_frame.size(); ++i) {
            const long vt = t.vt_by_frame[i];
            const long fi = t.visit.firstFrame + static_cast<long>(i);
            if (std::abs(vt - max_vt_ / 3)     < bestA) { bestA = std::abs(vt - max_vt_ / 3);     frameA = fi; }
            if (std::abs(vt - 2 * max_vt_ / 3) < bestB) { bestB = std::abs(vt - 2 * max_vt_ / 3); frameB = fi; }
        }
    }
    if (frameA < 0 || frameB < 0 || frameA == frameB) return;

    auto read_frame = [&](long index) {
        VideoSource video(video_path_);
        VideoSource::Frame f;
        if (!video.ok() || !video.skip_to(index) || !video.next(f)) return cv::Mat();
        return f.bgr;
    };
    const cv::Mat a = read_frame(frameA), b2 = read_frame(frameB);
    if (a.empty() || b2.empty()) return;

    for (const cv::Rect& b : candidates) {
        // Compare the element's OWN pixels — the confirmed-fixed ones inside its
        // box — not the whole box. A round element in a square box has
        // background corners (~21% of the area for a disc), and by construction
        // the two frames show DIFFERENT content behind it, so those corners
        // alone can sink a genuine overlay: measured on rw3, the blue
        // chat-launcher disc beside the "Haben Sie Fragen?" pill scored MAD 42
        // against a threshold of 18 purely from its corners. It was rejected and
        // stayed baked into the scroll content at every position it was seen.
        const cv::Mat m = fixed_mask_(b);
        const int n = cv::countNonZero(m);
        if (n <= 0) continue;
        const double mad = cv::norm(a(b), b2(b), cv::NORM_L1, m) / (3.0 * n);
        if (mad >= SAD_THRESHOLD) {           // content moved: not an overlay
            if (verbose_)
                std::cout << "[page " << page_id_ << " overlay rejected] "
                          << b.width << "×" << b.height << " at viewport ("
                          << b.x << "," << b.y << ")  verifyMAD=" << mad << "\n";
            continue;
        }
        add_overlay(b);
        if (verbose_)
            std::cout << "[page " << page_id_ << " overlay] "
                      << b.width << "×" << b.height << " at viewport ("
                      << b.x << "," << b.y << ")  verifyMAD=" << mad << "\n";
    }
}

// ── stamp ─────────────────────────────────────────────────────────────────────
// Paste one fixed band onto the final canvas at its document position. Header and
// sidebars are sourced from the shallowest-scroll frame, the footer from the
// deepest-scroll frame, so that each band's (possibly over-detected) margin
// lines up with the correct adjacent content.
void PageStitcher::stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const
{
    const cv::Rect& r = fr.rect;
    const cv::Mat& src_frame = (fr.kind == FixedKind::Footer) ? last_frame_ : first_frame_;
    if (src_frame.empty()) return;
    const cv::Mat src = src_frame(r);

    switch (fr.kind) {
    case FixedKind::Footer: {
        // Bottom-anchored: keep its distance from the viewport bottom. Extended
        // upward by the interior margin (correct content in the deepest frame).
        const int ext = std::min(INTERIOR_ROW_MARGIN, r.y);
        const cv::Rect band(r.x, r.y - ext, r.width, r.height + ext);
        int dst_y = static_cast<int>(doc_h) - (frame_h_ - band.y);
        if (dst_y >= 0)
            src_frame(band).copyTo(canvas(cv::Rect(band.x, dst_y, band.width, band.height)));
        break;
    }
    case FixedKind::Left:
    case FixedKind::Right: {
        // A fixed sidebar is present at every scroll offset, so it spans the
        // whole document height. The strip's bottom rows are trimmed by the
        // interior margin — an under-detected footer leaves its drop shadow
        // right above the band, and replicating a shadowed row would paint a
        // dark column down the page. The row above the trim is shadow-free.
        const int trim = std::min(INTERIOR_ROW_MARGIN, r.height - 1);
        const cv::Rect band(r.x, r.y, r.width, r.height - trim);
        src_frame(band).copyTo(canvas(cv::Rect(band.x, band.y, band.width, band.height)));

        const int fill_top    = band.y + band.height;
        const int fill_bottom = static_cast<int>(doc_h) - (frame_h_ - (r.y + r.height));
        const int fill_count  = fill_bottom - fill_top;
        if (fill_count > 0) {
            cv::Mat tail;
            cv::repeat(src_frame.row(band.y + band.height - 1).colRange(r.x, r.x + r.width),
                       fill_count, 1, tail);
            tail.copyTo(canvas(cv::Rect(r.x, fill_top, r.width, fill_count)));
        }
        break;
    }
    case FixedKind::Header:
    default: {
        // Anchored at the top. Extended downward by the interior margin
        // (correct content in the shallowest frame).
        const int ext = std::min(INTERIOR_ROW_MARGIN, frame_h_ - (r.y + r.height));
        const cv::Rect band(r.x, r.y, r.width, r.height + ext);
        src_frame(band).copyTo(canvas(cv::Rect(band.x, band.y, band.width, band.height)));
        break;
    }
    }
}

// ── reconstruct (merge all visits) ────────────────────────────────────────────
cv::Mat PageStitcher::reconstruct()
{
    const long doc_h = doc_height();

    // The scroll content is the interior rectangle inside the fixed bands.
    // Copying it wholesale (rather than masking the raw fixed pixels) avoids both
    // holes from false-positive fixed pixels and the need for a per-frame mask.
    // Rows are cropped INTERIOR_ROW_MARGIN inside a DETECTED band so a slightly
    // under-detected band cannot smear chrome residue across the stitch; the
    // extended header/footer stamps close the margin at the document ends.
    // Without a detected band there is no residue to guard against — and no
    // stamp to cover the margin (a no-scroll visit would get blank edge rows).
    const int top_m = header_h_ > 0 ? INTERIOR_ROW_MARGIN : 0;
    const int bot_m = footer_h_ > 0 ? INTERIOR_ROW_MARGIN : 0;
    // The scrollbar columns are excluded from the scrolling interior and painted
    // separately: the thumb moves with the scroll position, so that column is not
    // document content at any offset. Measured on rw4's deep page before this:
    // 92% of the column ended up blank white (an overlay bbox covered it, so the
    // blank-fill pass could never recover it) with 120 rows of thumb remnants.
    const cv::Rect interior(left_w_, header_h_ + top_m,
                            frame_w_ - left_w_ - right_w_ - sb_w_,
                            frame_h_ - header_h_ - footer_h_ - top_m - bot_m);
    if (interior.width <= 0 || interior.height <= 0)
        return cv::Mat(static_cast<int>(doc_h), frame_w_, CV_8UC3, cv::Scalar(255, 255, 255));

    // Overlay column masks per interior row: a row crossing an overlay copies
    // only the columns outside it. Those masks depend on the VIEWPORT row, not
    // the document row, so they are identical in every frame — which is what
    // keeps the row signatures below comparable. Content under an overlay is
    // recovered by the blank-fill pass from frames where that document row sat
    // elsewhere in the viewport.
    std::vector<cv::Mat> row_mask(interior.height);   // empty = full row
    for (const cv::Rect& o : overlays_) {
        for (int fy = std::max(o.y, interior.y);
             fy < std::min(o.y + o.height, interior.y + interior.height); ++fy) {
            cv::Mat& m = row_mask[fy - interior.y];
            if (m.empty()) m = cv::Mat(1, interior.width, CV_8UC1, cv::Scalar(255));
            const int a = std::max(0, o.x - interior.x);
            const int b = std::min(interior.width, o.x + o.width - interior.x);
            if (a < b) m.colRange(a, b).setTo(0);
        }
    }

    // ── Per-row modal reconciliation ─────────────────────────────────────────
    // Every observation of a document row votes; the appearance most frames
    // agree on wins, and ties go to the LATER frame because a page settles as
    // time passes. See types.hpp (ROW_SIG_*) for why first-observation freezing
    // was wrong and what it cost. A continuously animating region has no
    // majority — every observation differs — and still collapses to ONE coherent
    // instant, which is exactly what the old freeze achieved and why it existed.
    // The vote is scoped to ONE VISIT, and an earlier visit's rows are never
    // overwritten by a later one. Settling is a within-visit phenomenon;
    // differences BETWEEN visits are genuine content changes — a page that
    // reloaded is a new document version, not a noisy observation of the old one
    // — so a per-row majority across visits would blend two versions into an
    // incoherent canvas. Measured: voting across visits dropped rw1's
    // reload-REVISIT vote concentration from 0.526 to 0.429, under the 0.5 bar,
    // splitting that page in two.
    using RowSig = std::array<uint8_t, ROW_SIG_BINS * 3>;
    struct Candidate { RowSig sig{}; int count = 0; long frame = -1; };
    struct RowVotes  { Candidate slot[ROW_VOTE_SLOTS]; };

    // Bucketed column means of one interior row. Unmasked rows (the common case)
    // go through an INTER_AREA resize, which IS the per-bucket mean; a masked
    // row is averaged by hand over its visible columns only. A row is either
    // always masked or never, so the two paths never describe the same row.
    auto signature = [&](const cv::Mat& row, const cv::Mat& mask, RowSig& out) {
        if (mask.empty()) {
            cv::Mat small;
            cv::resize(row, small, cv::Size(ROW_SIG_BINS, 1), 0, 0, cv::INTER_AREA);
            std::memcpy(out.data(), small.data, out.size());
            return true;
        }
        const uint8_t* p = row.ptr<uint8_t>(0);
        const uint8_t* m = mask.ptr<uint8_t>(0);
        int used = 0;
        for (int b = 0; b < ROW_SIG_BINS; ++b) {
            const int x0 = b * interior.width / ROW_SIG_BINS;
            const int x1 = (b + 1) * interior.width / ROW_SIG_BINS;
            long acc[3] = {0, 0, 0};
            int n = 0;
            for (int x = x0; x < x1; ++x) {
                if (!m[x]) continue;
                acc[0] += p[3 * x]; acc[1] += p[3 * x + 1]; acc[2] += p[3 * x + 2];
                ++n;
            }
            used += n;
            for (int c = 0; c < 3; ++c)
                out[static_cast<size_t>(b * 3 + c)] =
                    n ? static_cast<uint8_t>(acc[c] / n) : 0;
        }
        return used * 2 >= interior.width;   // too little visible to judge
    };
    auto sig_diff = [](const RowSig& a, const RowSig& b) {
        int acc = 0;
        for (size_t i = 0; i < a.size(); ++i)
            acc += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        return static_cast<double>(acc) / static_cast<double>(a.size());
    };
    auto offset_of = [](const VisitTrace& t, long frame_index) {
        const long local = frame_index - t.visit.firstFrame;
        const long vt = local < static_cast<long>(t.vt_by_frame.size())
                            ? t.vt_by_frame[static_cast<size_t>(local)]
                            : (t.vt_by_frame.empty() ? 0 : t.vt_by_frame.back());
        return std::max(0L, vt);
    };

    // ── Incremental folding ──────────────────────────────────────────────────
    // reconstruct() is called after EVERY visit, so rebuilding the whole page
    // each time re-reads every earlier visit: cost grows as V(V+1)/2 in the
    // visits per page. The content buffer, its written map and the count of
    // visits already folded are therefore PERSISTENT, and a call folds only the
    // visits it has not seen yet.
    //
    // A fold is only valid while the geometry it was made under still holds. The
    // interior rect and the overlay set are inputs to every write (they decide
    // which columns are copied at all), so if either changed since the last fold
    // the earlier writes are stale and everything is refolded. Growth of the
    // document is not a change of geometry — the buffer is extended and the
    // existing rows keep their document coordinates.
    const bool geometry_changed = content_.empty()
                               || interior != folded_interior_
                               || overlays_ != folded_overlays_;
    if (geometry_changed) {
        content_     = cv::Mat(static_cast<int>(doc_h), frame_w_, CV_8UC3, cv::Scalar(255, 255, 255));
        written_     = cv::Mat::zeros(static_cast<int>(doc_h), interior.width, CV_8UC1);
        row_written_.assign(static_cast<size_t>(doc_h), 0);
        folded_      = 0;
    } else if (doc_h > content_.rows) {
        cv::Mat grown(static_cast<int>(doc_h), frame_w_, CV_8UC3, cv::Scalar(255, 255, 255));
        content_.copyTo(grown(cv::Rect(0, 0, frame_w_, content_.rows)));
        content_ = grown;
        cv::Mat grown_w = cv::Mat::zeros(static_cast<int>(doc_h), interior.width, CV_8UC1);
        written_.copyTo(grown_w(cv::Rect(0, 0, interior.width, written_.rows)));
        written_ = grown_w;
        row_written_.resize(static_cast<size_t>(doc_h), 0);
    }
    if (verbose_ && folded_ < visits_.size())
        std::cout << "[page " << page_id_ << " fold] visits " << folded_ << ".."
                  << visits_.size() - 1 << " of " << visits_.size()
                  << (geometry_changed ? "  (geometry changed: full refold)" : "  (incremental)")
                  << "\n";

    for (size_t vi = folded_; vi < visits_.size(); ++vi) {
      const VisitTrace& trace = visits_[vi];
      // Pass A — count the appearances of every document row IN THIS VISIT.
      std::vector<RowVotes> votes(static_cast<size_t>(doc_h));
      {
        VideoSource video = open_at(trace.visit);
        if (!video.ok()) continue;

        VideoSource::Frame f;
        while (video.next(f) && f.index <= trace.visit.lastFrame) {
            const int dst_y = static_cast<int>(offset_of(trace, f.index)) + interior.y;
            if (dst_y + interior.height > static_cast<int>(doc_h)) continue;   // safety

            const cv::Mat incoming = f.bgr(interior);
            for (int row = 0; row < interior.height; ++row) {
                RowSig sig;
                if (!signature(incoming.row(row), row_mask[row], sig)) continue;
                RowVotes& v = votes[static_cast<size_t>(dst_y + row)];
                int match = -1, free_slot = -1;
                for (int k = 0; k < ROW_VOTE_SLOTS; ++k) {
                    if (v.slot[k].count <= 0) { if (free_slot < 0) free_slot = k; continue; }
                    if (sig_diff(v.slot[k].sig, sig) <= ROW_SIG_MATCH) { match = k; break; }
                }
                if (match >= 0) {
                    ++v.slot[match].count;
                    v.slot[match].frame = f.index;   // latest exemplar of this appearance
                } else if (free_slot >= 0) {
                    v.slot[free_slot] = {sig, 1, f.index};
                } else {
                    // More distinct appearances than slots — the classic
                    // majority-of-k step: weaken every candidate, which frees a
                    // slot for whichever appearance keeps recurring.
                    for (int k = 0; k < ROW_VOTE_SLOTS; ++k) --v.slot[k].count;
                }
            }
        }
      }

      // The frame that won each document row (-1 = no vote: more than half the
      // row is permanently overlay-masked, so any frame of this visit will do).
      std::vector<long> winner(static_cast<size_t>(doc_h), -1);
      for (long y = 0; y < doc_h; ++y) {
          const RowVotes& v = votes[static_cast<size_t>(y)];
          int best = -1;
          for (int k = 0; k < ROW_VOTE_SLOTS; ++k) {
              if (v.slot[k].count <= 0) continue;
              if (best < 0 || v.slot[k].count > v.slot[best].count
                  || (v.slot[k].count == v.slot[best].count
                      && v.slot[k].frame > v.slot[best].frame))
                  best = k;
          }
          if (best >= 0) winner[static_cast<size_t>(y)] = v.slot[best].frame;
      }

      // Pass B — write this visit's rows from their winning frames, leaving rows
      // an earlier visit already filled untouched.
      {
        VideoSource video = open_at(trace.visit);
        if (!video.ok()) continue;

        VideoSource::Frame f;
        while (video.next(f) && f.index <= trace.visit.lastFrame) {
            const int dst_y = static_cast<int>(offset_of(trace, f.index)) + interior.y;
            if (dst_y + interior.height > static_cast<int>(doc_h)) continue;   // safety

            const cv::Mat incoming = f.bgr(interior);
            for (int row = 0; row < interior.height; ++row) {
                const int doc_row = dst_y + row;
                if (row_written_[doc_row]) continue;         // earlier visit owns it
                if (winner[static_cast<size_t>(doc_row)] >= 0
                    && winner[static_cast<size_t>(doc_row)] != f.index) continue;

                const cv::Mat& mask = row_mask[row];
                cv::Mat dst  = content_(cv::Rect(interior.x, doc_row, interior.width, 1));
                cv::Mat wrow = written_.row(doc_row);
                if (mask.empty()) { incoming.row(row).copyTo(dst);       wrow.setTo(255); }
                else              { incoming.row(row).copyTo(dst, mask); wrow.setTo(255, mask); }
                row_written_[static_cast<size_t>(doc_row)] = 1;
            }
        }
      }

      // Pass C — blank-fill: interior pixels still unwritten sat behind a
      // viewport-fixed overlay in the frame that won their document row. Recover
      // them from any frame of THIS visit that shows the same document row at a
      // viewport position clear of the overlays — a blank pixel has nothing to
      // disagree with, so the first sighting wins. Running it per visit is
      // equivalent to one pass at the end (visits are folded in order, so the
      // first sighting is the same), and it is what keeps the fold incremental.
      if (!overlays_.empty()) {
        const long before = static_cast<long>(interior.width) * doc_h - cv::countNonZero(written_);
        VideoSource video = open_at(trace.visit);
        if (video.ok()) {
            VideoSource::Frame f;
            while (video.next(f) && f.index <= trace.visit.lastFrame) {
                const int dst_y = static_cast<int>(offset_of(trace, f.index)) + interior.y;
                if (dst_y + interior.height > static_cast<int>(doc_h)) continue;

                const cv::Mat incoming = f.bgr(interior);
                for (int row = 0; row < interior.height; ++row) {
                    const int doc_row = dst_y + row;
                    cv::Mat wrow = written_.row(doc_row);
                    cv::Mat fillable;
                    cv::bitwise_not(wrow, fillable);          // 255 where still blank
                    const cv::Mat& mask = row_mask[row];      // overlay cols in THIS frame
                    if (!mask.empty()) cv::bitwise_and(fillable, mask, fillable);
                    if (cv::countNonZero(fillable) == 0) continue;
                    cv::Mat dst = content_(cv::Rect(interior.x, doc_row, interior.width, 1));
                    incoming.row(row).copyTo(dst, fillable);
                    cv::bitwise_or(wrow, fillable, wrow);
                }
            }
        }
        if (verbose_) {
            const long after = static_cast<long>(interior.width) * doc_h - cv::countNonZero(written_);
            std::cout << "[page " << page_id_ << " blank-fill] unwritten "
                      << before << " -> " << after << " interior px\n";
        }
      }
    }   // end of the per-visit fold

    folded_          = visits_.size();
    folded_interior_ = interior;
    folded_overlays_ = overlays_;

    // Compose the output: the folded content, then the chrome on top. The stamps
    // are re-applied on every call because they depend on the CURRENT document
    // height (the footer, bottom-anchored overlays and the scrollbar's lower
    // arrow are all positioned from the bottom) — which is exactly why they are
    // kept out of the persistent content buffer.
    cv::Mat canvas = content_.clone();

    // ── Rows with no observed scrolling content ──────────────────────────────
    // Two permanent causes, both DECLARED (report `gaps`) rather than presented
    // as reconstructed page content — the same honesty rule as the fixed bands'
    // uncertainty. (1) A jump beyond the shared-pixel window skipped the rows
    // outright; nothing can recover pixels never captured, and they stay blank.
    // (2) A viewport-fixed element covered them at every reachable offset — a
    // full-width sticky nav hides the document rows beneath it forever (measured
    // on rw3: doc rows 72..175) — and the stitch shows the stamped element there.
    // Only the scrolling span is examined: the rows above/below it belong to the
    // header and footer stamps, which cover them by construction.
    gaps_.clear();
    {
        const long content_top = interior.y;
        const long content_bot = doc_h - (frame_h_ - (interior.y + interior.height));
        long run_start = -1;
        for (long y = content_top; y <= content_bot && content_bot > content_top; ++y) {
            const bool blank = y < content_bot
                               && cv::countNonZero(written_.row(static_cast<int>(y))) == 0;
            if (blank && run_start < 0) run_start = y;
            if (!blank && run_start >= 0) {
                if (y - run_start >= GAP_MIN_ROWS) gaps_.push_back({run_start, y});
                run_start = -1;
            }
        }
        if (verbose_ && !gaps_.empty()) {
            long total = 0;
            for (const Gap& g : gaps_) total += g.to - g.from;
            std::cout << "[page " << page_id_ << " gaps] " << gaps_.size()
                      << " never-observed range(s), " << total << "px total\n";
        }
    }

    // Stamp the fixed chrome on top: sidebars first, then header/footer, so the
    // corners resolve as header/footer-over-sidebar.
    for (FixedKind k : { FixedKind::Left, FixedKind::Right, FixedKind::Header, FixedKind::Footer })
        for (const FixedRect& fr : rects_)
            if (fr.kind == k) stamp(canvas, fr, doc_h);

    // Stamp each overlay ONCE, anchored to the viewport edge it sits nearest
    // (a bottom-pinned floating button keeps its distance from the bottom of
    // the final screenshot, like the footer). Source: the frame nearest that
    // edge of the document, where the overlay is guaranteed on-screen.
    for (const cv::Rect& o : overlays_) {
        const bool bottom_anchored = o.y + o.height / 2 > frame_h_ / 2;
        const cv::Mat& src_frame = bottom_anchored ? last_frame_ : first_frame_;
        if (src_frame.empty()) continue;
        const int dst_y = bottom_anchored
                              ? static_cast<int>(doc_h) - (frame_h_ - o.y)
                              : o.y;
        if (dst_y < 0 || dst_y + o.height > static_cast<int>(doc_h)) continue;
        src_frame(o).copyTo(canvas(cv::Rect(o.x, dst_y, o.width, o.height)));
    }

    // Last, so the bar wins wherever an overlay bbox reached the edge — which is
    // also what the viewport showed.
    stamp_scrollbar(canvas, doc_h);

    return canvas;
}

// ── stamp_scrollbar ───────────────────────────────────────────────────────────
// The vertical scrollbar is viewport CHROME, not document content: its track and
// arrow buttons are position-fixed, and its thumb encodes the scroll position,
// which is meaningless in document coordinates — the same reason the recorder
// hides position:fixed elements for its full-page reference render. So the
// column is painted from the thumb-free strip: arrows at both document ends
// (like the header and footer bands) with clean track in between.
//
// Known degradation: on a page that barely scrolled the thumb may never vacate
// some rows, and those rows keep it. Harmless — a thumb filling most of its
// track is what such a page actually showed.
void PageStitcher::stamp_scrollbar(cv::Mat& canvas, long doc_h) const
{
    if (sb_w_ <= 0 || sb_clean_.empty() || doc_h < frame_h_) return;
    const int x    = frame_w_ - sb_w_;
    const int half = frame_h_ / 2;
    if (half <= 0 || half >= frame_h_) return;

    cv::Mat track;   // a mid-track row: guaranteed thumb-free after the maximum
    cv::repeat(sb_clean_.row(half), static_cast<int>(doc_h), 1, track);
    track.copyTo(canvas(cv::Rect(x, 0, sb_w_, static_cast<int>(doc_h))));

    sb_clean_.rowRange(0, half).copyTo(canvas(cv::Rect(x, 0, sb_w_, half)));
    const int tail = frame_h_ - half;
    sb_clean_.rowRange(half, frame_h_)
        .copyTo(canvas(cv::Rect(x, static_cast<int>(doc_h) - tail, sb_w_, tail)));
}
