#pragma once
#include "global_canvas.hpp"

struct RendererOptions {
    bool draw_grid = false;
    bool verbose   = false;
};

// Composes the final output image from a finished GlobalCanvas.
//
// Layout (top to bottom):
//   ┌─────────────────────────┐
//   │  Fixed header region    │
//   ├─────────────────────────┤
//   │  Stitched scroll area   │
//   ├─────────────────────────┤
//   │  Fixed footer region    │
//   └─────────────────────────┘
class Renderer {
public:
    explicit Renderer(RendererOptions opts = RendererOptions{}) : opts_(opts) {}

    [[nodiscard]] cv::Mat render(const GlobalCanvas& canvas) const;

private:
    RendererOptions opts_;

    struct FixedRegions {
        int header_rows = 0;
        int footer_rows = 0;
        int left_cols   = 0;
        int right_cols  = 0;
    };

    FixedRegions detect_fixed_regions(const cv::Mat& mask,
                                       int grid_rows, int grid_cols) const;

    // Horizontal strip (header / footer): full-width, gy_start..gy_end viewport rows.
    cv::Mat build_fixed_row_strip(const GlobalCanvas& canvas,
                                   int gy_start, int gy_end) const;

    // Vertical strip (left / right sidebar): gx_start..gx_end viewport cols,
    // gy_start..gy_end rows.  Height = (gy_end-gy_start)*BLOCK_SIZE.
    cv::Mat build_fixed_col_strip(const GlobalCanvas& canvas,
                                   int gx_start, int gx_end,
                                   int gy_start, int gy_end) const;

    void overlay_debug_grid(cv::Mat& img) const;
};
