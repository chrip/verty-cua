#include "page_stitcher.hpp"
#include <algorithm>
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
    calibrate(curr_gray, vt);

    while (video.next(f) && f.index <= visit.lastFrame) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray,
                                             fixed_mask_, noise_mask_);
        if (tr.type == TransitionType::Scroll)
            vt = std::max(0L, vt + tr.dy);

        trace.vt_by_frame.push_back(vt);
        track_extremes(vt, f.bgr);
        calibrate(curr_gray, vt);
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
        const double mad = cv::norm(a(b), b2(b), cv::NORM_L1) / (3.0 * b.area());
        if (mad >= SAD_THRESHOLD) continue;   // content moved: not an overlay
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
    cv::Mat canvas(static_cast<int>(doc_h), frame_w_, CV_8UC3, cv::Scalar(255, 255, 255));

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
    const cv::Rect interior(left_w_, header_h_ + top_m,
                            frame_w_ - left_w_ - right_w_,
                            frame_h_ - header_h_ - footer_h_ - top_m - bot_m);
    if (interior.width <= 0 || interior.height <= 0) return canvas;

    // Conflict-frozen writes: a document row, once stitched, is refreshed by a
    // later frame only if the frames agree on it — where "agree" tolerates a
    // small vertical drift (a row matching the canvas within ±2px is scroll
    // accumulation error, not new content, and refreshing keeps each write
    // self-consistent). A row that matches at NO nearby offset holds dynamic
    // content (animation, a floating button over changing text) and is frozen
    // at its first observation instead of being smeared across the page.
    // Overlay column masks per interior row: a row crossing an overlay copies
    // (and compares) only the columns outside it. Content under the overlay is
    // filled by frames where that document row was NOT under it — the overlay
    // is viewport-fixed while the content scrolls past.
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

    auto row_mad = [&](const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask) {
        const double n = mask.empty() ? interior.width : cv::countNonZero(mask);
        if (n <= 0) return 0.0;
        return cv::norm(a, b, cv::NORM_L1, mask) / (3.0 * n);
    };
    auto explainable = [&](const cv::Mat& incoming_row, int doc_row, const cv::Mat& mask) {
        for (int s = -2; s <= 2; ++s) {
            const int y = doc_row + s;
            if (y < 0 || y >= static_cast<int>(doc_h)) continue;
            if (row_mad(incoming_row, canvas(cv::Rect(interior.x, y, interior.width, 1)), mask)
                    <= ROW_CONFLICT_MAD)
                return true;
        }
        return false;
    };
    // Per-pixel written map over the interior columns: 255 where a real content
    // pixel has been placed. Overlay columns start unwritten (masked out below),
    // which drives the blank-fill pass.
    cv::Mat written = cv::Mat::zeros(static_cast<int>(doc_h), interior.width, CV_8UC1);
    std::vector<uint8_t> row_written(static_cast<size_t>(doc_h), 0);
    for (const VisitTrace& trace : visits_) {
        VideoSource video = open_at(trace.visit);
        if (!video.ok()) continue;

        VideoSource::Frame f;
        while (video.next(f) && f.index <= trace.visit.lastFrame) {
            const long local = f.index - trace.visit.firstFrame;
            long vt = local < static_cast<long>(trace.vt_by_frame.size())
                          ? trace.vt_by_frame[local]
                          : (trace.vt_by_frame.empty() ? 0 : trace.vt_by_frame.back());
            vt = std::max(0L, vt);
            const int dst_y = static_cast<int>(vt) + interior.y;
            if (dst_y + interior.height > static_cast<int>(doc_h)) continue;   // safety

            const cv::Mat incoming = f.bgr(interior);
            for (int row = 0; row < interior.height; ++row) {
                const int doc_row = dst_y + row;
                const cv::Mat& mask = row_mask[row];
                cv::Mat dst = canvas(cv::Rect(interior.x, doc_row, interior.width, 1));
                if (row_written[doc_row]
                    && row_mad(incoming.row(row), dst, mask) > ROW_CONFLICT_MAD
                    && !explainable(incoming.row(row), doc_row, mask))
                    continue;   // dynamic content: keep the first observation
                cv::Mat wrow = written.row(doc_row);
                if (mask.empty()) { incoming.row(row).copyTo(dst);       wrow.setTo(255); }
                else              { incoming.row(row).copyTo(dst, mask); wrow.setTo(255, mask); }
                row_written[doc_row] = 1;
            }
        }
    }

    // Blank-fill pass: interior pixels still unwritten sat behind a
    // viewport-fixed overlay in EVERY frame that stitched their document row
    // (the conflict-freeze also refuses to fill a blank column, seeing the
    // white gap as disagreement). Recover them from any frame that shows the
    // same document row at a viewport position clear of the overlays — a blank
    // pixel has nothing to conflict with, so this pass takes the first sighting.
    // Only needed when overlays created masked columns.
    if (!overlays_.empty()) {
        const long before = static_cast<long>(interior.width) * doc_h - cv::countNonZero(written);
        for (const VisitTrace& trace : visits_) {
            VideoSource video = open_at(trace.visit);
            if (!video.ok()) continue;

            VideoSource::Frame f;
            while (video.next(f) && f.index <= trace.visit.lastFrame) {
                const long local = f.index - trace.visit.firstFrame;
                long vt = local < static_cast<long>(trace.vt_by_frame.size())
                              ? trace.vt_by_frame[local]
                              : (trace.vt_by_frame.empty() ? 0 : trace.vt_by_frame.back());
                vt = std::max(0L, vt);
                const int dst_y = static_cast<int>(vt) + interior.y;
                if (dst_y + interior.height > static_cast<int>(doc_h)) continue;

                const cv::Mat incoming = f.bgr(interior);
                for (int row = 0; row < interior.height; ++row) {
                    const int doc_row = dst_y + row;
                    cv::Mat wrow = written.row(doc_row);
                    cv::Mat fillable;
                    cv::bitwise_not(wrow, fillable);          // 255 where still blank
                    const cv::Mat& mask = row_mask[row];      // overlay cols in THIS frame
                    if (!mask.empty()) cv::bitwise_and(fillable, mask, fillable);
                    if (cv::countNonZero(fillable) == 0) continue;
                    cv::Mat dst = canvas(cv::Rect(interior.x, doc_row, interior.width, 1));
                    incoming.row(row).copyTo(dst, fillable);
                    cv::bitwise_or(wrow, fillable, wrow);
                }
            }
        }
        if (verbose_) {
            const long after = static_cast<long>(interior.width) * doc_h - cv::countNonZero(written);
            std::cout << "[page " << page_id_ << " blank-fill] unwritten "
                      << before << " -> " << after << " interior px\n";
        }
    }

    // ── Never-observed rows ──────────────────────────────────────────────────
    // A jump beyond the shared-pixel window skips document content outright: no
    // frame ever showed those rows, so nothing can recover them. They stay blank
    // and are DECLARED (report `gaps`) rather than presented as page content —
    // the same honesty rule as the fixed bands' uncertainty. Only the scrolling
    // span is examined: the rows above/below it belong to the header and footer
    // stamps, which cover them by construction.
    gaps_.clear();
    {
        const long content_top = interior.y;
        const long content_bot = doc_h - (frame_h_ - (interior.y + interior.height));
        long run_start = -1;
        for (long y = content_top; y <= content_bot && content_bot > content_top; ++y) {
            const bool blank = y < content_bot
                               && cv::countNonZero(written.row(static_cast<int>(y))) == 0;
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

    return canvas;
}
