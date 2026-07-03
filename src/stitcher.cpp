#include "stitcher.hpp"
#include <algorithm>
#include <iostream>

// ── construction ──────────────────────────────────────────────────────────────
Stitcher::Stitcher(std::string video_path, bool verbose)
    : video_path_(std::move(video_path)), verbose_(verbose)
{
    VideoSource probe(video_path_);
    if (!probe.ok()) return;
    frame_w_ = probe.width();
    frame_h_ = probe.height();
    fps_     = probe.fps();
    if (frame_w_ <= 0 || frame_h_ <= 0) return;

    stable_counter_ = cv::Mat::zeros(frame_h_, frame_w_, CV_32SC1);
    fixed_mask_     = cv::Mat::zeros(frame_h_, frame_w_, CV_8UC1);
    ok_ = true;
}

cv::Mat Stitcher::run()
{
    if (!ok_) return {};
    analyze();
    refine_scrolls();
    return reconstruct();
}

// ── accumulate_fixed ────────────────────────────────────────────────────────
// A pixel's stability counter increments while it stays unchanged across a
// scroll transition and resets to 0 the moment it changes. Truly position-fixed
// pixels (header/footer/sidebars) survive every scroll; scrolling content does
// not. A light 3×3 blur suppresses single-pixel compression noise.
void Stitcher::accumulate_fixed(const cv::Mat& prev_gray, const cv::Mat& curr_gray)
{
    cv::Mat diff, smooth;
    cv::absdiff(prev_gray, curr_gray, diff);
    cv::blur(diff, smooth, cv::Size(3, 3));

    cv::Mat stable = smooth < SAD_THRESHOLD;   // CV_8U, 255 where unchanged
    stable_counter_.setTo(0, ~stable);         // reset the pixels that moved
    cv::add(stable_counter_, 1, stable_counter_, stable);   // ++ the stable ones
}

// ── analyze (PASS 1) ──────────────────────────────────────────────────────────
// Accumulate the fixed-pixel mask and a rough per-frame scroll offset.
void Stitcher::analyze()
{
    VideoSource video(video_path_);
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    vt_by_frame_.clear();
    max_vt_ = 0;

    if (!video.next(f)) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();
    f.bgr.copyTo(first_frame_);   // scroll top
    f.bgr.copyTo(last_frame_);

    long vt = 0;
    vt_by_frame_.push_back(vt);   // frame 0 sits at the document top

    while (video.next(f)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray);  // no mask yet
        if (tr.type == TransitionType::Scroll) {
            accumulate_fixed(prev_gray, curr_gray);
            vt = std::max(0L, vt + tr.dy);
        }

        vt_by_frame_.push_back(vt);
        if (vt >= max_vt_) { max_vt_ = vt; f.bgr.copyTo(last_frame_); }
        prev_gray = curr_gray.clone();
    }

    cv::compare(stable_counter_, FIXED_CONFIRM_FRAMES, fixed_mask_, cv::CMP_GE);
    extract_rects();

    if (verbose_) {
        std::cout << "[analyze] frames=" << vt_by_frame_.size()
                  << "  maxScroll=" << max_vt_ << "px"
                  << "  docHeight=" << doc_height() << "px\n";
        std::cout << "[analyze] fixed header: " << header_h_ << "px"
                  << "  footer: " << footer_h_ << "px"
                  << "  left: "   << left_w_   << "px"
                  << "  right: "  << right_w_  << "px\n";
    }
}

// ── refine_scrolls (PASS 2) ─────────────────────────────────────────────────
// Re-classify every transition, telling the detector which pixels are fixed so
// it samples only genuine scrolling content. Overwrites the rough pass-1
// offsets; this pass's classifications are the session's authoritative ones
// (transition counts, scroll events).
void Stitcher::refine_scrolls()
{
    if (fixed_mask_.empty()) return;

    VideoSource video(video_path_);
    if (!video.ok()) return;

    VideoSource::Frame f;
    cv::Mat curr_gray, prev_gray;
    if (!video.next(f)) return;
    cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
    prev_gray = curr_gray.clone();

    std::vector<long> vt_by_frame(1, 0);   // frame 0 at the top
    long vt = 0, max_vt = 0;
    counts_ = {};

    while (video.next(f)) {
        cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);

        Transition tr = classifier_.classify(prev_gray, curr_gray, fixed_mask_);
        counts_.add(tr.type);
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
        std::cout << "[refine] mask-aware maxScroll=" << max_vt_
                  << "px  docHeight=" << doc_height() << "px"
                  << "  scrollEvents=" << scroll_events_.size()
                  << "  pageChanges=" << counts_.pageChange
                  << "  unstable=" << counts_.unstable << "\n";
}

// ── build_scroll_events ───────────────────────────────────────────────────────
// The scroll-event trace is derived from the refined per-frame viewport tops:
// every frame whose vt differs from its predecessor is one scroll step.
void Stitcher::build_scroll_events()
{
    scroll_events_.clear();
    for (size_t i = 1; i < vt_by_frame_.size(); ++i) {
        long dy = vt_by_frame_[i] - vt_by_frame_[i - 1];
        if (dy == 0) continue;
        scroll_events_.push_back({
            static_cast<long>(i),
            static_cast<double>(i) / fps_,
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
void Stitcher::extract_rects()
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
void Stitcher::stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const
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
cv::Mat Stitcher::reconstruct()
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

    VideoSource video(video_path_);
    if (!video.ok()) return canvas;

    // Stitch: place each frame's interior at its recorded scroll offset.
    VideoSource::Frame f;
    while (video.next(f)) {
        long vt = f.index < frame_count()
                      ? vt_by_frame_[f.index]
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
