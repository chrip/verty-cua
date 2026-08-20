#include "live/region_history.hpp"

#include "perception/types.hpp"

void RegionHistory::ensure_grid(const cv::Mat& gray)
{
    const int cols = gray.cols / BLOCK_SIZE;
    const int rows = gray.rows / BLOCK_SIZE;
    if (cols == cols_ && rows == rows_) return;
    // A resolution change invalidates every stored appearance: block (3,7) is
    // no longer the same part of the screen.
    cols_ = cols;
    rows_ = rows;
    states_.assign(static_cast<size_t>(cols_) * rows_, {});
}

bool RegionHistory::seen_before(int bx, int by, const cv::Mat& block) const
{
    const auto& states = states_[static_cast<size_t>(by) * cols_ + bx];
    for (const cv::Mat& s : states)
        if (patch_mad_at(s, 0, 0, block) <= mad_max_) return true;
    return false;
}

void RegionHistory::remember(int bx, int by, const cv::Mat& block)
{
    auto& states = states_[static_cast<size_t>(by) * cols_ + bx];
    if (static_cast<int>(states.size()) >= max_states_) return;
    states.push_back(block.clone());
}

void RegionHistory::observe(const cv::Mat& gray)
{
    ensure_grid(gray);
    for (int by = 0; by < rows_; ++by)
        for (int bx = 0; bx < cols_; ++bx) {
            const cv::Mat b = extract_block(gray, bx * BLOCK_SIZE, by * BLOCK_SIZE);
            if (!b.empty() && !seen_before(bx, by, b)) remember(bx, by, b);
        }
}

cv::Mat RegionHistory::classify_and_observe(const cv::Mat& after_gray,
                                            const cv::Mat& changed,
                                            RegionChangeSummary& out)
{
    ensure_grid(after_gray);
    cv::Mat labels(rows_, cols_, CV_8U, cv::Scalar(0));
    out = RegionChangeSummary{};

    for (int by = 0; by < rows_; ++by) {
        for (int bx = 0; bx < cols_; ++bx) {
            const cv::Rect r(bx * BLOCK_SIZE, by * BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
            if (r.x + r.width > changed.cols || r.y + r.height > changed.rows) continue;

            // Only blocks the System-0 mask already calls changed are asked
            // about; the rest carry no question.
            if (cv::countNonZero(changed(r)) == 0) continue;

            const cv::Mat b = extract_block(after_gray, r.x, r.y);
            if (b.empty()) continue;

            const bool known = seen_before(bx, by, b);
            labels.at<unsigned char>(by, bx) =
                static_cast<unsigned char>(known ? BlockClass::Revisit : BlockClass::Novel);
            if (known) ++out.revisit_blocks;
            else       { ++out.novel_blocks; remember(bx, by, b); }
            ++out.changed_blocks;
        }
    }
    return labels;
}

cv::Mat RegionHistory::expand(const cv::Mat& block_labels, cv::Size frame_size)
{
    cv::Mat full;
    cv::resize(block_labels, full, frame_size, 0, 0, cv::INTER_NEAREST);
    return full;
}
