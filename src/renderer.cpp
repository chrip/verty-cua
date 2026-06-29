#include "renderer.hpp"
#include <algorithm>
#include <iostream>

// ── detect_fixed_regions ──────────────────────────────────────────────────────
// Scans the fixed_mask for contiguous runs of fully-fixed rows/columns at each
// edge.  A row is "fully fixed" only if every column in that row is fixed, and
// vice versa for columns.  Regions are detected independently so a corner block
// that is fixed due to header AND sidebar overlap is counted in both.
Renderer::FixedRegions Renderer::detect_fixed_regions(const cv::Mat& mask,
                                                        int grid_rows,
                                                        int grid_cols) const
{
    FixedRegions fr;
    if (mask.empty()) return fr;

    // Header: contiguous top rows where ALL columns are fixed
    for (int gy = 0; gy < grid_rows; ++gy) {
        bool all = true;
        for (int gx = 0; gx < grid_cols && all; ++gx)
            if (mask.at<uint8_t>(gy, gx) != 255) all = false;
        if (all) ++fr.header_rows; else break;
    }

    // Footer: contiguous bottom rows where ALL columns are fixed
    for (int gy = grid_rows - 1; gy >= fr.header_rows; --gy) {
        bool all = true;
        for (int gx = 0; gx < grid_cols && all; ++gx)
            if (mask.at<uint8_t>(gy, gx) != 255) all = false;
        if (all) ++fr.footer_rows; else break;
    }

    // Left sidebar: contiguous leftmost columns where ALL rows are fixed
    for (int gx = 0; gx < grid_cols; ++gx) {
        bool all = true;
        for (int gy = 0; gy < grid_rows && all; ++gy)
            if (mask.at<uint8_t>(gy, gx) != 255) all = false;
        if (all) ++fr.left_cols; else break;
    }

    // Right sidebar: contiguous rightmost columns where ALL rows are fixed
    for (int gx = grid_cols - 1; gx >= fr.left_cols; --gx) {
        bool all = true;
        for (int gy = 0; gy < grid_rows && all; ++gy)
            if (mask.at<uint8_t>(gy, gx) != 255) all = false;
        if (all) ++fr.right_cols; else break;
    }

    return fr;
}

// ── build_fixed_row_strip ─────────────────────────────────────────────────────
// Horizontal strip spanning the full frame width, gy_start..gy_end rows.
cv::Mat Renderer::build_fixed_row_strip(const GlobalCanvas& canvas,
                                         int gy_start, int gy_end) const
{
    const auto& fc = canvas.fixed_content();
    int strip_h = (gy_end - gy_start) * BLOCK_SIZE;
    int strip_w = canvas.doc_width();
    cv::Mat strip(strip_h, strip_w, CV_8UC3, cv::Scalar(200, 200, 200));

    for (int gy = gy_start; gy < gy_end; ++gy) {
        for (int gx = 0; gx < canvas.grid_cols(); ++gx) {
            auto it = fc.find({gx, gy});
            if (it == fc.end()) continue;
            int dx = gx * BLOCK_SIZE;
            int dy = (gy - gy_start) * BLOCK_SIZE;
            it->second.copyTo(strip(cv::Rect(dx, dy, BLOCK_SIZE, BLOCK_SIZE)));
        }
    }
    return strip;
}

// ── build_fixed_col_strip ─────────────────────────────────────────────────────
// Vertical strip for gx_start..gx_end columns, gy_start..gy_end rows.
// Produces one full viewport-height slice of the fixed sidebar element.
cv::Mat Renderer::build_fixed_col_strip(const GlobalCanvas& canvas,
                                         int gx_start, int gx_end,
                                         int gy_start, int gy_end) const
{
    const auto& fc = canvas.fixed_content();
    int strip_w = (gx_end - gx_start) * BLOCK_SIZE;
    int strip_h = (gy_end - gy_start) * BLOCK_SIZE;
    cv::Mat strip(strip_h, strip_w, CV_8UC3, cv::Scalar(200, 200, 200));

    for (int gy = gy_start; gy < gy_end; ++gy) {
        for (int gx = gx_start; gx < gx_end; ++gx) {
            auto it = fc.find({gx, gy});
            if (it == fc.end()) continue;
            int dx = (gx - gx_start) * BLOCK_SIZE;
            int dy = (gy - gy_start) * BLOCK_SIZE;
            it->second.copyTo(strip(cv::Rect(dx, dy, BLOCK_SIZE, BLOCK_SIZE)));
        }
    }
    return strip;
}

// ── overlay_debug_grid ────────────────────────────────────────────────────────
void Renderer::overlay_debug_grid(cv::Mat& img) const
{
    const cv::Scalar colour(0, 255, 0);
    for (int x = 0; x < img.cols; x += BLOCK_SIZE)
        cv::line(img, {x, 0}, {x, img.rows - 1}, colour, 1);
    for (int y = 0; y < img.rows; y += BLOCK_SIZE)
        cv::line(img, {0, y}, {img.cols - 1, y}, colour, 1);
}

// ── render ────────────────────────────────────────────────────────────────────
cv::Mat Renderer::render(const GlobalCanvas& canvas) const
{
    const cv::Mat& mask = canvas.fixed_mask();
    int grid_rows = canvas.grid_rows();
    int grid_cols = canvas.grid_cols();

    FixedRegions fr = detect_fixed_regions(mask, grid_rows, grid_cols);

    if (opts_.verbose) {
        std::cout << "[renderer] fixed header rows: " << fr.header_rows
                  << "  footer rows: " << fr.footer_rows
                  << "  left cols: "   << fr.left_cols
                  << "  right cols: "  << fr.right_cols << "\n"
                  << "[renderer] doc height: " << canvas.doc_height() << "px\n";
    }

    // ── 1. Build horizontal strips (header / footer) ──────────────────────────
    cv::Mat header_img = (fr.header_rows > 0)
        ? build_fixed_row_strip(canvas, 0, fr.header_rows) : cv::Mat();

    cv::Mat footer_img = (fr.footer_rows > 0)
        ? build_fixed_row_strip(canvas, grid_rows - fr.footer_rows, grid_rows)
        : cv::Mat();

    // ── 2. Build stitched scroll area ─────────────────────────────────────────
    cv::Mat scroll_img = canvas.stitch_scroll();
    if (scroll_img.empty()) {
        std::cerr << "[renderer] no scrollable content collected\n";
        return {};
    }

    // Crop only the TOP of the stitch (header rows).
    // Header blocks ARE stored in scroll_content_ via ingest_first() and their
    // doc positions duplicate what the header strip already renders.
    //
    // Footer blocks are NOT stored in scroll_content_ — once promoted to
    // fixed_content_ they are no longer written to the stitch, so the stitch
    // naturally terminates before the footer's doc positions.  Cropping
    // crop_bottom would remove legitimate content that scrolled to those
    // doc positions, not footer pixels.
    {
        int crop_top  = fr.header_rows * BLOCK_SIZE;
        int remaining = scroll_img.rows - crop_top;
        if (remaining > 0)
            scroll_img = scroll_img(
                cv::Rect(0, crop_top, scroll_img.cols, remaining)).clone();
    }

    if (opts_.draw_grid) overlay_debug_grid(scroll_img);

    // ── 3. Stack vertically: header + scroll + footer ─────────────────────────
    int out_h = scroll_img.rows
              + (header_img.empty() ? 0 : header_img.rows)
              + (footer_img.empty() ? 0 : footer_img.rows);
    int out_w = scroll_img.cols;

    cv::Mat out(out_h, out_w, CV_8UC3, cv::Scalar(255, 255, 255));

    int y = 0;
    auto paste_row = [&](const cv::Mat& src) {
        if (src.empty()) return;
        int w = std::min(src.cols, out.cols);
        int h = std::min(src.rows, out_h - y);
        if (h <= 0) return;
        src(cv::Rect(0, 0, w, h)).copyTo(out(cv::Rect(0, y, w, h)));
        y += src.rows;
    };
    paste_row(header_img);
    paste_row(scroll_img);
    paste_row(footer_img);

    // ── 4. Overlay fixed sidebars ─────────────────────────────────────────────
    // Each sidebar is exactly one viewport-height tall.  It is pasted at:
    //   • x: its column position in the frame
    //   • y: top of the header (or top of output if no header)
    // The sidebar strip covers gy = [header_rows .. grid_rows-footer_rows),
    // i.e. the rows not already rendered by the header / footer strips.
    // The header and footer strips include the sidebar columns at those rows
    // (because build_fixed_row_strip renders the full width from fixed_content_),
    // so we only need to fill the middle section.
    int scroll_y_offset = (header_img.empty() ? 0 : header_img.rows);
    int gy_inner_start  = fr.header_rows;
    int gy_inner_end    = grid_rows - fr.footer_rows;

    // scroll region: between header and footer
    int scroll_area_top    = scroll_y_offset;
    int scroll_area_bottom = out_h - (footer_img.empty() ? 0 : footer_img.rows);
    int scroll_area_h      = scroll_area_bottom - scroll_area_top;

    auto paste_col_strip = [&](int gx_start, int gx_end) {
        if (gx_start >= gx_end) return;
        int dst_x   = gx_start * BLOCK_SIZE;
        int strip_w = (gx_end - gx_start) * BLOCK_SIZE;
        if (dst_x + strip_w > out.cols) return;

        // Erase only the SCROLL area in those columns — not header or footer.
        // Pre-promotion frames may have leaked banner pixels into the stitch.
        if (scroll_area_h > 0)
            out(cv::Rect(dst_x, scroll_area_top, strip_w, scroll_area_h))
                .setTo(cv::Scalar(235, 235, 235));

        // Paste the clean one-viewport-height sidebar strip into the scroll area.
        cv::Mat strip = build_fixed_col_strip(
            canvas, gx_start, gx_end, gy_inner_start, gy_inner_end);
        if (strip.empty()) return;
        int h = std::min(strip.rows, scroll_area_h);
        if (h > 0)
            strip(cv::Rect(0, 0, strip_w, h)).copyTo(
                out(cv::Rect(dst_x, scroll_area_top, strip_w, h)));
    };

    if (fr.left_cols  > 0) paste_col_strip(0, fr.left_cols);
    if (fr.right_cols > 0) paste_col_strip(grid_cols - fr.right_cols, grid_cols);

    return out;
}
