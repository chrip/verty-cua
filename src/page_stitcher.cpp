#include "page_stitcher.hpp"
#include <algorithm>
#include <iostream>

// ── construction ──────────────────────────────────────────────────────────────
PageStitcher::PageStitcher(std::string video_path, PageVisit visit,
                           int frame_w, int frame_h, double fps,
                           bool verbose, int page_id)
    : video_path_(std::move(video_path)), visit_(visit),
      frame_w_(frame_w), frame_h_(frame_h), fps_(fps),
      verbose_(verbose), page_id_(page_id),
      accumulator_(frame_w, frame_h)
{
    fixed_mask_ = cv::Mat::zeros(frame_h_, frame_w_, CV_8UC1);
}

VideoSource PageStitcher::open_at_visit() const
{
    VideoSource video(video_path_);
    if (video.ok()) video.skip_to(visit_.firstFrame);
    return video;
}

cv::Mat PageStitcher::run()
{
    if (frame_w_ <= 0 || frame_h_ <= 0) return {};
    analyze();
    refine_scrolls();
    return reconstruct();
}

// ── analyze (PASS 1) ──────────────────────────────────────────────────────────
// Accumulate the fixed-pixel mask and a rough per-frame scroll offset.
void PageStitcher::analyze()
{
    VideoSource video = open_at_visit();
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    vt_by_frame_.clear();
    max_vt_ = 0;

    if (!video.next(f) || !in_visit(f.index)) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();
    f.bgr.copyTo(first_frame_);   // scroll top
    f.bgr.copyTo(last_frame_);

    long vt = 0;
    vt_by_frame_.push_back(vt);   // the visit's first frame sits at the doc top

    while (video.next(f) && in_visit(f.index)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray);  // no mask yet
        if (tr.type == TransitionType::Scroll) {
            accumulator_.accumulate(prev_gray, curr_gray);
            vt = std::max(0L, vt + tr.dy);
        }

        vt_by_frame_.push_back(vt);
        if (vt >= max_vt_) { max_vt_ = vt; f.bgr.copyTo(last_frame_); }
        prev_gray = curr_gray.clone();
    }

    fixed_mask_ = accumulator_.mask();
    extract_rects();

    if (verbose_) {
        std::cout << "[page " << page_id_ << " analyze] frames=" << vt_by_frame_.size()
                  << "  maxScroll=" << max_vt_ << "px"
                  << "  docHeight=" << doc_height() << "px\n";
        std::cout << "[page " << page_id_ << " analyze] fixed header: " << header_h_ << "px"
                  << "  footer: " << footer_h_ << "px"
                  << "  left: "   << left_w_   << "px"
                  << "  right: "  << right_w_  << "px\n";
    }
}

// ── refine_scrolls (PASS 2) ─────────────────────────────────────────────────
// Re-classify every transition of the visit, telling the detector which pixels
// are fixed so it samples only genuine scrolling content. Overwrites the rough
// pass-1 offsets with the authoritative ones.
void PageStitcher::refine_scrolls()
{
    if (fixed_mask_.empty()) return;

    VideoSource video = open_at_visit();
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    if (!video.next(f) || !in_visit(f.index)) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();

    std::vector<long> vt_by_frame(1, 0);
    long vt = 0, max_vt = 0;

    while (video.next(f) && in_visit(f.index)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray, fixed_mask_);
        if (tr.type == TransitionType::Scroll)
            vt = std::max(0L, vt + tr.dy);

        vt_by_frame.push_back(vt);
        max_vt = std::max(max_vt, vt);
        prev_gray = curr_gray.clone();
    }

    vt_by_frame_ = std::move(vt_by_frame);
    max_vt_      = max_vt;
    build_scroll_events();

    if (verbose_)
        std::cout << "[page " << page_id_ << " refine] mask-aware maxScroll=" << max_vt_
                  << "px  docHeight=" << doc_height() << "px"
                  << "  scrollEvents=" << scroll_events_.size() << "\n";
}

// ── build_scroll_events ───────────────────────────────────────────────────────
// The scroll-event trace is derived from the refined per-frame viewport tops:
// every frame whose vt differs from its predecessor is one scroll step.
// Frame indices and timestamps are global (session coordinates).
void PageStitcher::build_scroll_events()
{
    scroll_events_.clear();
    for (size_t i = 1; i < vt_by_frame_.size(); ++i) {
        long dy = vt_by_frame_[i] - vt_by_frame_[i - 1];
        if (dy == 0) continue;
        const long frame = visit_.firstFrame + static_cast<long>(i);
        scroll_events_.push_back({
            frame,
            static_cast<double>(frame) / fps_,
            static_cast<int>(dy),
            vt_by_frame_[i],
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

// ── stamp ─────────────────────────────────────────────────────────────────────
// Paste one fixed band onto the final canvas at its document position. Header and
// sidebars are sourced from the top frame, the footer from the bottom frame, so
// that each band's (possibly over-detected) margin lines up with the correct
// adjacent content.
void PageStitcher::stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const
{
    const cv::Rect& r = fr.rect;
    const cv::Mat& src_frame = (fr.kind == FixedKind::Footer) ? last_frame_ : first_frame_;
    if (src_frame.empty()) return;
    const cv::Mat src = src_frame(r);

    switch (fr.kind) {
    case FixedKind::Footer: {
        // Bottom-anchored: keep its distance from the viewport bottom.
        int dst_y = static_cast<int>(doc_h) - (frame_h_ - r.y);
        if (dst_y >= 0)
            src.copyTo(canvas(cv::Rect(r.x, dst_y, r.width, r.height)));
        break;
    }
    case FixedKind::Left:
    case FixedKind::Right: {
        // A fixed sidebar is present at every scroll offset, so it spans the whole
        // document height. Paste the captured strip, then replicate its bottom row
        // (sidebar background) down to the footer line.
        src.copyTo(canvas(cv::Rect(r.x, r.y, r.width, r.height)));
        int fill_bottom = static_cast<int>(doc_h) - (frame_h_ - (r.y + r.height));
        int fill_count  = fill_bottom - (r.y + r.height);
        if (fill_count > 0) {
            cv::Mat tail;
            cv::repeat(src.row(r.height - 1), fill_count, 1, tail);
            tail.copyTo(canvas(cv::Rect(r.x, r.y + r.height, r.width, fill_count)));
        }
        break;
    }
    case FixedKind::Header:
    default:
        // Anchored where it was seen in the viewport (the top).
        src.copyTo(canvas(cv::Rect(r.x, r.y, r.width, r.height)));
        break;
    }
}

// ── reconstruct (PASS 3) ──────────────────────────────────────────────────────
cv::Mat PageStitcher::reconstruct()
{
    const long doc_h = doc_height();
    cv::Mat canvas(static_cast<int>(doc_h), frame_w_, CV_8UC3, cv::Scalar(255, 255, 255));

    // The scroll content is the interior rectangle inside the fixed bands.
    // Copying it wholesale (rather than masking the raw fixed pixels) avoids both
    // holes from false-positive fixed pixels and the need for a per-frame mask.
    const cv::Rect interior(left_w_, header_h_,
                            frame_w_ - left_w_ - right_w_,
                            frame_h_ - header_h_ - footer_h_);
    if (interior.width <= 0 || interior.height <= 0) return canvas;

    VideoSource video = open_at_visit();
    if (!video.ok()) return canvas;

    // Stitch: place each frame's interior at its recorded scroll offset.
    VideoSource::Frame f;
    while (video.next(f) && in_visit(f.index)) {
        const long local = f.index - visit_.firstFrame;
        long vt = local < static_cast<long>(vt_by_frame_.size())
                      ? vt_by_frame_[local]
                      : (vt_by_frame_.empty() ? 0 : vt_by_frame_.back());
        vt = std::max(0L, vt);
        int dst_y = static_cast<int>(vt) + interior.y;
        if (dst_y + interior.height > static_cast<int>(doc_h)) continue;   // safety
        f.bgr(interior).copyTo(canvas(cv::Rect(interior.x, dst_y, interior.width, interior.height)));
    }

    // Stamp the fixed chrome on top: sidebars first, then header/footer, so the
    // corners resolve as header/footer-over-sidebar.
    for (FixedKind k : { FixedKind::Left, FixedKind::Right, FixedKind::Header, FixedKind::Footer })
        for (const FixedRect& fr : rects_)
            if (fr.kind == k) stamp(canvas, fr, doc_h);

    return canvas;
}
