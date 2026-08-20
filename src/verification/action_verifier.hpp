#pragma once
#include <string>

#include "live/visual_delta.hpp"
#include "verification/action.hpp"

// ── The missing layer ────────────────────────────────────────────────────────
// Verty knows WHAT changed. It does not know what was SUPPOSED to change,
// because it never knew an action was taken. This is that layer: action +
// visual delta -> is this the outcome that action should have produced?
//
// Design rule, non-negotiable: Unknown is the safe default. A wrong `Expected`
// makes an agent proceed on a false premise and silently fails the task; a
// wrong `Unknown` merely spends a VLM call that could have been saved. The
// rules below therefore only claim Expected where the evidence is
// unambiguous, and everything else falls through.

enum class Verdict {
    Expected,    // the change is what this action should produce -> no VLM turn
    Unexpected,  // the change contradicts the action -> stop, hand to the VLM
    Unknown,     // not confidently decidable -> hand to the VLM
};

// How large the change is, in the bands M0 measured. The boundaries are the
// empty gaps in the observed distribution, not round numbers.
enum class ChangeExtent { None, Trace, Local, Large, Global };

struct VerificationResult {
    Verdict      verdict = Verdict::Unknown;
    ChangeExtent extent  = ChangeExtent::None;
    std::string  reason;      // why, in words, for the replay log
};

// Context carried across steps. Everything here is optional: with a default
// value the verifier behaves exactly as the stateless M1 version did, which is
// what keeps the two comparable.
struct VerifierContext {
    // Where the last coordinate-bearing action landed. Keyboard actions have no
    // coordinate of their own and act on whatever that click focused.
    bool      has_focus = false;
    cv::Point focus;

    // Locality of this step's change measured at `focus` rather than at the
    // action's own coordinate. Only meaningful when has_focus is set.
    ChangeLocality focus_locality;
};

// Thresholds. Every default here is measured on the M0 corpus
// a 106-step corpus of a collaborative office suite in a browser, with
// NO SCROLLING IN IT. The scroll rules below are therefore written from the
// action's semantics, not fitted -- they are the least trustworthy part of this
// file until a corpus with scrolling exists.
struct VerifierConfig {
    // Observed: 12 steps changed exactly 0 pixels; the next smallest change was
    // well clear of it. A pure zero is the only safe "nothing happened".
    double none_max = 1e-6;

    // Observed: a band of tiny changes (cursor blink, focus ring, hover) with
    // no semantic weight.
    double trace_max = 1e-3;

    // Observed: local UI changes -- dialogs, menus, typed text, list rows --
    // topped out at 0.0126 of the frame. Nothing landed between 0.0126 and
    // 0.0364, and nothing between 0.0364 and 0.107.
    double local_max = 0.03;

    // Observed: whole-screen changes started at 0.107. The gap between local_max
    // and this is deliberately left as Large -- an ambiguous middle that always
    // returns Unknown rather than guessing.
    double global_min = 0.05;

    // A click's change should be near the click. Measured: 76% of clicks that
    // changed anything sat inside the change's bounding box, median distance 0px.
    // The tail is real though -- a click that opens a centred modal produces its
    // change away from the pointer -- so distance alone cannot refute a click.
    int    click_radius = 120;
    double click_far_px = 400.0;

    // How near the inherited focus point a keyboard action's change must sit
    // before it counts as "this key acted on the focused control". Deliberately
    // wider than click_radius: a dropdown opened by a click extends well below
    // it, and arrow-key navigation moves a highlight down that list.
    int    key_focus_radius = 260;

    // A changed pixel touching the click is necessary but NOT sufficient. A
    // document edit lights up the text caret AND the dirty-marker in the tab
    // title AND the window title AND the Save and Undo buttons -- seven
    // fragments spanning 84% of the frame, of which 11% sits near the pointer.
    // That is not "a click did a local thing", and a human reviewer flagged it
    // as such.
    //
    // So a click's change must also be CONCENTRATED: either most of it is near
    // the click, or it is structurally simple (one or two regions -- a menu, a
    // dialog, a highlight), which is how a legitimately off-pointer change such
    // as a centred modal still passes.
    double click_min_local_share = 0.20;
    int    click_max_scattered_components = 2;

    // A scroll's measured dy should agree with what was requested. Generous,
    // because smooth-scrolling and clamping at a document end both make the
    // realized amount differ from the request.
    double scroll_tolerance_frac = 0.5;
    int    scroll_tolerance_min_px = 40;
};

VerificationResult verify_action(const Action& action, const VisualDelta& delta,
                                 const ChangeLocality& locality,
                                 const VerifierConfig& cfg = {},
                                 const VerifierContext& ctx = {});

const char* verdict_name(Verdict v);
const char* extent_name(ChangeExtent e);
ChangeExtent classify_extent(double changed_frac, const VerifierConfig& cfg);
