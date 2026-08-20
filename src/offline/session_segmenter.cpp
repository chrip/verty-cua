#include "offline/session_segmenter.hpp"

SessionSegmenter::Result
SessionSegmenter::segment(const std::vector<Transition>& transitions) const
{
    Result r;
    const long n = static_cast<long>(transitions.size());
    if (n == 0) return r;

    auto is_stable = [](TransitionType t) {
        return t == TransitionType::Identical || t == TransitionType::Scroll;
    };

    long start = 0;   // first frame of the current visit
    long i = 1;
    while (i < n) {
        if (transitions[i].type != TransitionType::PageChange) { ++i; continue; }

        // Close the current visit at the last frame of the old page.
        if (i - 1 >= start)
            r.visits.push_back({start, i - 1});

        // Transition window: the new visit starts at the first frame of a run
        // of PAGE_STABLE_CONFIRM stable transitions. Any PageChange/Unstable
        // inside the window restarts the candidate (still rendering).
        long candidate = i;
        int  stable    = 0;
        long k         = i + 1;
        for (; k < n && stable < PAGE_STABLE_CONFIRM; ++k) {
            if (is_stable(transitions[k].type)) {
                ++stable;
            } else {
                candidate = k;
                stable    = 0;
            }
        }

        start = candidate;
        r.navFrames.push_back(candidate);
        i = k;
    }

    if (start <= n - 1)
        r.visits.push_back({start, n - 1});

    return r;
}
