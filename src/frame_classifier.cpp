#include "frame_classifier.hpp"
#include <queue>
#include <array>

// ── pass1 ─────────────────────────────────────────────────────────────────────
// Seed each block with an initial classification based on the dominant dy.
void FrameClassifier::pass1(const cv::Mat& prev_gray,
                             const cv::Mat& curr_gray,
                             int scroll_dy,
                             const cv::Mat& fixed_mask,
                             Grid& grid) const
{
    const int rows = static_cast<int>(grid.size());
    const int cols = rows ? static_cast<int>(grid[0].size()) : 0;

    for (int gy = 0; gy < rows; ++gy) {
        for (int gx = 0; gx < cols; ++gx) {
            Block& b  = grid[gy][gx];
            int px    = gx * BLOCK_SIZE;
            int py    = gy * BLOCK_SIZE;

            // Already confirmed FIXED by canvas history → respect it
            if (!fixed_mask.empty()) {
                int fy = gy < fixed_mask.rows ? gy : fixed_mask.rows - 1;
                int fx = gx < fixed_mask.cols ? gx : fixed_mask.cols - 1;
                if (fixed_mask.at<uint8_t>(fy, fx) == 255) {
                    b.cls = BlockClass::FIXED;
                    continue;
                }
            }

            // When scrolling: test SCROLLED first so that uniform-background
            // blocks (which trivially match at dy=0 too) don't get promoted
            // as fixed-element candidates.
            if (scroll_dy != 0 &&
                blocks_match(prev_gray, curr_gray, px, py, scroll_dy)) {
                b.cls = BlockClass::SCROLLED;
                b.scroll_dy = scroll_dy;
                continue;
            }

            // Check STATIC: matches prev at dy=0 but NOT at scroll_dy →
            // this block didn't move with the page (true fixed element).
            if (blocks_match(prev_gray, curr_gray, px, py, 0)) {
                b.cls = BlockClass::STATIC;
                b.scroll_dy = 0;
                continue;
            }

            b.cls = BlockClass::UNKNOWN;
        }
    }
}

// ── pass2 ─────────────────────────────────────────────────────────────────────
// BFS from seeded SCROLLED blocks.  Grow the region to UNKNOWN neighbours
// that also match at the same dy.  Any remaining UNKNOWN blocks that match
// at dy=0 become STATIC; the rest become NEW.
void FrameClassifier::pass2(const cv::Mat& prev_gray,
                             const cv::Mat& curr_gray,
                             int /*scroll_dy*/,
                             Grid& grid) const
{
    const int rows = static_cast<int>(grid.size());
    const int cols = rows ? static_cast<int>(grid[0].size()) : 0;

    // Seed queue with all SCROLLED blocks found in pass 1
    std::queue<std::pair<int,int>> q;
    for (int gy = 0; gy < rows; ++gy)
        for (int gx = 0; gx < cols; ++gx)
            if (grid[gy][gx].cls == BlockClass::SCROLLED)
                q.push({gx, gy});

    // 4-connected BFS
    constexpr std::array<std::pair<int,int>, 4> dirs = {{
        {1,0}, {-1,0}, {0,1}, {0,-1}
    }};

    while (!q.empty()) {
        auto [gx, gy] = q.front(); q.pop();
        int dy = grid[gy][gx].scroll_dy;

        for (auto [dx, dy2] : dirs) {
            int nx = gx + dx, ny = gy + dy2;
            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
            Block& nb = grid[ny][nx];
            if (nb.cls != BlockClass::UNKNOWN) continue;

            int px = nx * BLOCK_SIZE;
            int py = ny * BLOCK_SIZE;

            if (blocks_match(prev_gray, curr_gray, px, py, dy)) {
                nb.cls       = BlockClass::SCROLLED;
                nb.scroll_dy = dy;
                q.push({nx, ny});
            }
        }
    }

    // Resolve remaining UNKNOWNs
    for (int gy = 0; gy < rows; ++gy) {
        for (int gx = 0; gx < cols; ++gx) {
            Block& b = grid[gy][gx];
            if (b.cls != BlockClass::UNKNOWN) continue;

            int px = gx * BLOCK_SIZE;
            int py = gy * BLOCK_SIZE;

            // One more static check (might have been missed in pass1 due to
            // ordering or fixed-mask gaps)
            if (blocks_match(prev_gray, curr_gray, px, py, 0)) {
                b.cls       = BlockClass::STATIC;
                b.scroll_dy = 0;
            } else {
                b.cls = BlockClass::NEW;
            }
        }
    }
}

// ── assign_doc_y ──────────────────────────────────────────────────────────────
// Compute the document-Y coordinate of each block's top-left corner.
//
// For SCROLLED blocks: the document position is viewport_top_after_scroll + py.
// We already have the new viewport top (doc_y_top) which is post-scroll, so
// every block's document Y is simply doc_y_top + py regardless of class.
// Fixed blocks keep their viewport position expressed as a negative sentinel
// (handled separately in GlobalCanvas).
void FrameClassifier::assign_doc_y(Grid& grid, long doc_y_top) const
{
    for (auto& row : grid) {
        for (Block& b : row) {
            if (b.cls == BlockClass::FIXED) {
                b.doc_y = -(b.gy * BLOCK_SIZE + 1);  // negative sentinel
            } else {
                b.doc_y = doc_y_top + static_cast<long>(b.gy) * BLOCK_SIZE;
            }
        }
    }
}

// ── classify ──────────────────────────────────────────────────────────────────
Grid FrameClassifier::classify(const cv::Mat& prev_gray,
                                const cv::Mat& curr_gray,
                                int scroll_dy,
                                const cv::Mat& fixed_mask,
                                long doc_y_top) const
{
    const int blocks_y = curr_gray.rows / BLOCK_SIZE;
    const int blocks_x = curr_gray.cols / BLOCK_SIZE;

    // Initialise grid with block coordinates
    Grid grid(blocks_y, std::vector<Block>(blocks_x));
    for (int gy = 0; gy < blocks_y; ++gy)
        for (int gx = 0; gx < blocks_x; ++gx) {
            grid[gy][gx].gx = gx;
            grid[gy][gx].gy = gy;
        }

    pass1(prev_gray, curr_gray, scroll_dy, fixed_mask, grid);
    pass2(prev_gray, curr_gray, scroll_dy, grid);
    assign_doc_y(grid, doc_y_top);

    return grid;
}
