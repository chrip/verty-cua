#include "subframe_stitcher.hpp"
#include "page_stitcher.hpp"
#include "transition_classifier.hpp"
#include "video_source.hpp"
#include <iostream>

SubframeStitcher::SubframeStitcher(std::string video_path, double fps, bool verbose)
    : video_path_(std::move(video_path)), fps_(fps), verbose_(verbose) {}

std::vector<SubframeStitcher::Doc>
SubframeStitcher::run(const cv::Rect& roi, long first_frame, long last_frame, int subframe_id,
                      const std::vector<bool>& doc_moved)
{
    std::vector<Doc> docs;
    if (roi.width < BLOCK_SIZE || roi.height < BLOCK_SIZE) return docs;

    // ── Pass 1: classify the subframe's own transitions ──────────────────────
    // The same classifier, reading a smaller frame. No chrome or noise mask is
    // passed: inside a subframe the equivalents would have to be learned from the
    // subframe's own scrolls, and this pass exists to find out whether there were
    // any. The scroll veto is maskless by design anyway.
    std::vector<TransitionType> kinds;   // one per transition, index = frame - first - 1
    {
        VideoSource video(video_path_, roi);
        if (!video.ok()) return docs;
        if (!video.skip_to(first_frame)) return docs;
        TransitionClassifier classifier(Sensitivity::Normal);
        VideoSource::Frame f;
        cv::Mat prev_gray, curr_gray;
        if (!video.next(f) || f.index > last_frame) return docs;
        cv::cvtColor(f.bgr, prev_gray, cv::COLOR_BGR2GRAY);
        while (video.next(f) && f.index <= last_frame) {
            cv::cvtColor(f.bgr, curr_gray, cv::COLOR_BGR2GRAY);
            kinds.push_back(classifier.classify(prev_gray, curr_gray).type);
            prev_gray = curr_gray.clone();
        }
    }
    if (kinds.empty()) return docs;

    // ── Split into inner documents ───────────────────────────────────────────
    // A PAGE_CHANGE inside the subframe is a new screen; SCROLL and IDENTICAL
    // continue the current one. The run's own scroll count decides whether it
    // needs stitching at all.
    struct Run { long first, last; int scrolls = 0; };
    std::vector<Run> runs;
    runs.push_back({first_frame, first_frame, 0});
    for (size_t i = 0; i < kinds.size(); ++i) {
        const long frame = first_frame + static_cast<long>(i) + 1;
        const bool moved = frame < static_cast<long>(doc_moved.size())
                           && doc_moved[static_cast<size_t>(frame)];
        if (moved || kinds[i] == TransitionType::PageChange) {
            runs.back().last = frame - 1;
            runs.push_back({frame, frame, 0});
            continue;
        }
        if (kinds[i] == TransitionType::Scroll) ++runs.back().scrolls;
        runs.back().last = frame;
    }

    // ── Pass 2: stitch the runs that moved ───────────────────────────────────
    for (const Run& r : runs) {
        if (r.scrolls < SUBFRAME_MIN_INNER_SCROLLS) continue;
        if (r.last - r.first + 1 < 2) continue;
        PageStitcher stitcher(video_path_, roi.width, roi.height, fps_,
                              false /* verbose: its page-level lines would mislead */,
                              subframe_id, nullptr, roi);
        PageVisit visit;
        visit.firstFrame = r.first;
        visit.lastFrame  = r.last;
        stitcher.add_visit(visit, 0);
        Doc d;
        d.first_frame = r.first;
        d.last_frame  = r.last;
        d.scrolls     = r.scrolls;
        d.image       = stitcher.reconstruct();
        d.extent      = stitcher.captured_extent();
        if (d.image.empty()) continue;
        if (verbose_)
            std::cout << "[subframe " << subframe_id << " inner] frames ["
                      << r.first << ".." << r.last << "]  " << r.scrolls
                      << " inner scrolls  → " << d.image.cols << "×" << d.image.rows
                      << "px of inner document (subframe is " << roi.width << "×"
                      << roi.height << ")\n";
        docs.push_back(std::move(d));
    }
    return docs;
}
