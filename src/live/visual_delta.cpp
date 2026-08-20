#include "live/visual_delta.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "perception/types.hpp"

namespace {

// Half-width of the window the nearest-change distance transform runs in. Sized
// past VerifierConfig::click_far_px (400px) so a "far" verdict is a measurement
// rather than a window artifact.
constexpr int kLocalityWindow = 420;

// Absolute per-pixel difference above which a change is "strong" rather than a
// wash. 60 of 255 is roughly the gap between a focus-dim and real content.
constexpr int kStrongDiff = 60;

// The changed mask, computed exactly as TransitionClassifier computes it
// internally: absdiff -> 3x3 blur -> SAD_THRESHOLD. The blur is what keeps
// single-pixel sensor noise and font antialiasing out of the mask.
cv::Mat changed_mask(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat diff, smooth, changed;
    cv::absdiff(a, b, diff);
    cv::blur(diff, smooth, cv::Size(3, 3));
    cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
    return changed;
}

}  // namespace

VisualDelta measure_delta(const cv::Mat& before_gray, const cv::Mat& after_gray,
                          TransitionClassifier& classifier)
{
    VisualDelta d;
    d.frame_w = after_gray.cols;
    d.frame_h = after_gray.rows;

    const cv::Mat changed = changed_mask(before_gray, after_gray);
    const double total = static_cast<double>(changed.total());
    const int n = cv::countNonZero(changed);
    d.changed_frac = n / total;

    // Amplitude-weighted counterpart: changed pixels that actually differ a lot.
    {
        cv::Mat raw, strong;
        cv::absdiff(before_gray, after_gray, raw);
        cv::compare(raw, kStrongDiff, strong, cv::CMP_GE);
        cv::bitwise_and(strong, changed, strong);
        d.changed_frac_strong = cv::countNonZero(strong) / total;
    }

    if (n > 0) {
        std::vector<cv::Point> pts;
        cv::findNonZero(changed, pts);
        d.bbox = cv::boundingRect(pts);
        d.bbox_frac = d.bbox.area() / total;

        // Closing first so a line of antialiased text does not shatter into one
        // component per glyph; the question is "how many places changed", not
        // "how many glyphs".
        cv::Mat closed, labels;
        cv::morphologyEx(changed, closed, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, {9, 9}));
        d.components = cv::connectedComponents(closed, labels, 8, CV_32S) - 1;
    }

    for (int y = 0; y + RESIDUAL_BAND_H <= changed.rows; y += RESIDUAL_BAND_H) {
        const cv::Rect band(0, y, changed.cols, RESIDUAL_BAND_H);
        ++d.bands_total;
        if (cv::countNonZero(changed(band)) / static_cast<double>(band.area())
            > BAND_CHANGED_MIN_FRAC)
            ++d.bands_changed;
    }

    const Transition t = classifier.classify(before_gray, after_gray);
    d.transition = t.type;
    d.dy = t.dy;
    return d;
}

ChangeLocality measure_locality(const cv::Mat& before_gray, const cv::Mat& after_gray,
                                cv::Point at, int radius, const VisualDelta& d)
{
    ChangeLocality loc;
    if (d.changed_frac <= 0.0) return loc;   // nothing to be near
    loc.valid = true;

    const cv::Mat changed = changed_mask(before_gray, after_gray);
    const int n = cv::countNonZero(changed);
    const cv::Rect frame(0, 0, changed.cols, changed.rows);
    const cv::Rect around =
        cv::Rect(at.x - radius, at.y - radius, 2 * radius, 2 * radius) & frame;
    if (around.area() > 0 && n > 0)
        loc.frac_within_r = cv::countNonZero(changed(around)) / static_cast<double>(n);

    loc.inside_bbox = d.bbox.contains(at);
    const double bdx = std::max({0, d.bbox.x - at.x, at.x - (d.bbox.x + d.bbox.width)});
    const double bdy = std::max({0, d.bbox.y - at.y, at.y - (d.bbox.y + d.bbox.height)});
    loc.dist_to_bbox = std::sqrt(bdx * bdx + bdy * bdy);

    // Distance to the nearest ACTUALLY changed pixel -- the honest locality
    // measure, and the only one the verifier should use.
    //
    // A distance transform answers this exactly, but running it over the whole
    // frame costs ~3ms, which is a third of the per-pair budget spent on a
    // question that only matters near the point. So it runs on a window instead:
    // any answer larger than the window is reported as "beyond the window",
    // which is all a caller can act on anyway.
    loc.dist_to_change = std::numeric_limits<double>::infinity();
    if (frame.contains(at)) {
        const int reach = std::max(radius, kLocalityWindow);
        const cv::Rect win =
            cv::Rect(at.x - reach, at.y - reach, 2 * reach, 2 * reach) & frame;
        if (win.area() > 0 && cv::countNonZero(changed(win)) > 0) {
            cv::Mat unchanged, dist;
            cv::bitwise_not(changed(win), unchanged);
            cv::distanceTransform(unchanged, dist, cv::DIST_L2, 3);
            loc.dist_to_change = dist.at<float>(at.y - win.y, at.x - win.x);
        } else {
            // Nothing changed within the window; the true distance is at least
            // its half-width.
            loc.dist_to_change = static_cast<double>(reach);
        }
    }
    return loc;
}
