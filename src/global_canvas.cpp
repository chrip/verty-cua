#include "global_canvas.hpp"
#include <algorithm>
#include <limits>

// ── ctor ──────────────────────────────────────────────────────────────────────
GlobalCanvas::GlobalCanvas(int frame_w, int frame_h)
    : frame_w_(frame_w), frame_h_(frame_h),
      grid_cols_(frame_w / BLOCK_SIZE),
      grid_rows_(frame_h / BLOCK_SIZE)
{
    static_counter_ = cv::Mat::zeros(grid_rows_, grid_cols_, CV_32SC1);
    fixed_mask_     = cv::Mat::zeros(grid_rows_, grid_cols_, CV_8UC1);
}

// ── ingest_first ──────────────────────────────────────────────────────────────
// Treat every block of the first frame as NEW content at doc_y = 0.
void GlobalCanvas::ingest_first(const cv::Mat& bgr)
{
    for (int gy = 0; gy < grid_rows_; ++gy) {
        for (int gx = 0; gx < grid_cols_; ++gx) {
            long doc_block_y = static_cast<long>(gy);
            auto key = std::make_pair(gx, doc_block_y);
            if (scroll_content_.find(key) == scroll_content_.end()) {
                int px = gx * BLOCK_SIZE, py = gy * BLOCK_SIZE;
                scroll_content_[key] =
                    bgr(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE)).clone();
            }
        }
    }
}

// ── promote_fixed_candidates ──────────────────────────────────────────────────
// Independently check each block for temporal stability at dy=0.  This runs
// directly on the raw grayscale frames so it is NOT confused by the
// SCROLLED/STATIC priority in the classifier (which can mis-classify
// uniform-colour blocks like solid headers as SCROLLED).
//
// A block is stable at dy=0 if curr(px,py) ≈ prev(px,py) regardless of
// how the classifier labelled it.  After FIXED_CONFIRM_FRAMES consecutive
// scroll events where the block is stable, it is promoted to the fixed mask.
void GlobalCanvas::promote_fixed_candidates(const cv::Mat& prev_gray,
                                             const cv::Mat& curr_gray,
                                             int scroll_dy)
{
    if (scroll_dy == 0) return;  // need an active scroll to distinguish fixed vs scrolled

    for (int gy = 0; gy < grid_rows_; ++gy) {
        for (int gx = 0; gx < grid_cols_; ++gx) {
            int px = gx * BLOCK_SIZE;
            int py = gy * BLOCK_SIZE;

            int32_t& cnt = static_counter_.at<int32_t>(gy, gx);

            // Directly test content stability at viewport position (dy=0).
            // Hard reset on ANY mismatch: a single disruption (e.g. a colored
            // content stripe scrolling past) clears the counter completely.
            // This prevents uniform background areas from false-accumulating
            // while still reliably promoting truly fixed elements (they NEVER
            // see a disrupting stripe — their stability is perfect).
            if (blocks_match(prev_gray, curr_gray, px, py, 0)) {
                cnt = std::min(cnt + 1, FIXED_CONFIRM_FRAMES);
            } else {
                cnt = 0;
            }

            fixed_mask_.at<uint8_t>(gy, gx) =
                (cnt >= FIXED_CONFIRM_FRAMES) ? 255 : 0;
        }
    }
}

// ── store_block ───────────────────────────────────────────────────────────────
void GlobalCanvas::store_block(const cv::Mat& bgr, const Block& b)
{
    int px = b.gx * BLOCK_SIZE;
    int py = b.gy * BLOCK_SIZE;
    cv::Mat patch = bgr(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE)).clone();

    if (b.cls == BlockClass::FIXED) {
        fixed_content_[{b.gx, b.gy}] = std::move(patch);
        return;
    }

    // doc_block_y: the block row index in document space
    long doc_block_y = b.doc_y / BLOCK_SIZE;
    auto key = std::make_pair(b.gx, doc_block_y);

    // Always overwrite: later frames have better resolution / less compression
    scroll_content_[key] = std::move(patch);
}

// ── update ────────────────────────────────────────────────────────────────────
void GlobalCanvas::update(const cv::Mat& bgr,
                          const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                          const Grid& grid, int scroll_dy)
{
    // Advance the viewport position
    viewport_top_ += scroll_dy;

    promote_fixed_candidates(prev_gray, curr_gray, scroll_dy);

    for (int gy = 0; gy < grid_rows_; ++gy) {
        for (int gx = 0; gx < grid_cols_; ++gx) {
            const Block& b = grid[gy][gx];

            // Re-check fixed mask: the classifier might not have the latest mask
            if (fixed_mask_.at<uint8_t>(gy, gx) == 255) {
                int px = b.gx * BLOCK_SIZE;
                int py = b.gy * BLOCK_SIZE;
                fixed_content_[{b.gx, b.gy}] =
                    bgr(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE)).clone();
                continue;
            }

            // Store scrollable content for any non-STATIC block (or STATIC at
            // doc position not yet seen)
            if (b.cls == BlockClass::SCROLLED || b.cls == BlockClass::NEW) {
                store_block(bgr, b);
            } else if (b.cls == BlockClass::STATIC) {
                // Fill in gaps: store if this doc position wasn't seen yet
                long doc_block_y = b.doc_y / BLOCK_SIZE;
                auto key = std::make_pair(b.gx, doc_block_y);
                if (scroll_content_.find(key) == scroll_content_.end()) {
                    store_block(bgr, b);
                }
            }
        }
    }
}

// ── doc_height ────────────────────────────────────────────────────────────────
long GlobalCanvas::doc_height() const
{
    if (scroll_content_.empty()) return frame_h_;

    long max_doc_block_y = 0;
    for (const auto& [key, _] : scroll_content_)
        max_doc_block_y = std::max(max_doc_block_y, key.second);

    return (max_doc_block_y + 1) * static_cast<long>(BLOCK_SIZE);
}

// ── stitch_scroll ─────────────────────────────────────────────────────────────
cv::Mat GlobalCanvas::stitch_scroll() const
{
    if (scroll_content_.empty()) return {};

    // Find document extent
    long min_block_y = std::numeric_limits<long>::max();
    long max_block_y = std::numeric_limits<long>::min();
    for (const auto& [key, _] : scroll_content_) {
        min_block_y = std::min(min_block_y, key.second);
        max_block_y = std::max(max_block_y, key.second);
    }

    long total_block_rows = max_block_y - min_block_y + 1;
    long total_height     = total_block_rows * BLOCK_SIZE;
    int  total_width      = grid_cols_ * BLOCK_SIZE;

    // Allocate output (grey default so unfilled areas are obvious)
    cv::Mat out(static_cast<int>(total_height), total_width,
                CV_8UC3, cv::Scalar(180, 180, 180));

    for (const auto& [key, patch] : scroll_content_) {
        auto [gx, doc_block_y] = key;
        long local_block_y     = doc_block_y - min_block_y;

        int dst_x = gx * BLOCK_SIZE;
        int dst_y = static_cast<int>(local_block_y * BLOCK_SIZE);

        if (dst_x + BLOCK_SIZE > out.cols || dst_y + BLOCK_SIZE > out.rows) continue;
        patch.copyTo(out(cv::Rect(dst_x, dst_y, BLOCK_SIZE, BLOCK_SIZE)));
    }

    return out;
}
