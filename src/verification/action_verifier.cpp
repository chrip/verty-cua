#include "verification/action_verifier.hpp"

#include <cmath>
#include <cstdlib>

const char* verdict_name(Verdict v)
{
    switch (v) {
    case Verdict::Expected:   return "expected";
    case Verdict::Unexpected: return "unexpected";
    case Verdict::Unknown:    return "unknown";
    }
    return "unknown";
}

const char* extent_name(ChangeExtent e)
{
    switch (e) {
    case ChangeExtent::None:   return "none";
    case ChangeExtent::Trace:  return "trace";
    case ChangeExtent::Local:  return "local";
    case ChangeExtent::Large:  return "large";
    case ChangeExtent::Global: return "global";
    }
    return "none";
}

ChangeExtent classify_extent(double f, const VerifierConfig& cfg)
{
    if (f <= cfg.none_max)   return ChangeExtent::None;
    if (f <= cfg.trace_max)  return ChangeExtent::Trace;
    if (f <= cfg.local_max)  return ChangeExtent::Local;
    if (f <  cfg.global_min) return ChangeExtent::Large;   // the ambiguous middle
    return ChangeExtent::Global;
}

namespace {

VerificationResult make(Verdict v, ChangeExtent e, std::string why)
{
    return {v, e, std::move(why)};
}

// A click, double-click or right-click. The dominant expected outcome is a
// local change; a navigation is also legitimate but indistinguishable from an
// accidental one without semantics, so it is handed up.
VerificationResult verify_click(const Action& a, const VisualDelta& d,
                                const ChangeLocality& loc,
                                const VerifierConfig& cfg, ChangeExtent extent)
{
    switch (extent) {
    case ChangeExtent::None:
        // The clearest signal in the whole corpus: the pointer went down and the
        // screen did not move. Six of 61 clicks did this. The click missed, or
        // hit something inert. Cheap, confident, and worth a full VLM turn saved
        // -- the agent must not proceed as if it had worked.
        return make(Verdict::Unexpected, extent,
                    "click produced no visible change at all");

    case ChangeExtent::Trace:
        // A focus ring, a hover highlight, a caret placed. Something acknowledged
        // the pointer -- but only if it happened WHERE the pointer is. A trace
        // change elsewhere is unrelated background noise and says nothing about
        // the click.
        if (a.has_coord && loc.valid
            && loc.dist_to_change <= static_cast<double>(cfg.click_radius))
            return make(Verdict::Expected, extent,
                        "trace change at the click point; the control acknowledged it");
        return make(Verdict::Unknown, extent,
                    "click produced only a trace change, and not at the click point");

    case ChangeExtent::Local: {
        if (!a.has_coord || !loc.valid)
            return make(Verdict::Unknown, extent,
                        "local change, but no coordinate to test it against");

        // Distance to the nearest changed PIXEL, not to the union bounding box.
        // The box test was tried first and is wrong: a change made of several
        // disjoint components has a union box covering most of the frame, so it
        // called a click on the ruler "expected" while the only real changes
        // were the window's modified-marker and a text line elsewhere.
        if (loc.dist_to_change <= static_cast<double>(cfg.click_radius)) {
            // Concentration test -- see click_min_local_share. Without it, a
            // document-edit cascade passes on the strength of a single caret
            // pixel at the click while the rest of the frame changes.
            if (loc.frac_within_r >= cfg.click_min_local_share
                || d.components <= cfg.click_max_scattered_components)
                return make(Verdict::Expected, extent,
                            "local change concentrated within "
                            + std::to_string(cfg.click_radius) + "px of the click");
            return make(Verdict::Unknown, extent,
                        "change touches the click but is scattered across the frame; "
                        "looks like a state cascade, not a local click result");
        }
        if (loc.dist_to_change > cfg.click_far_px)
            // Far away and small: a background update, a toast, an async render.
            // Not evidence the click did anything.
            return make(Verdict::Unknown, extent,
                        "local change far from the click; likely unrelated");
        return make(Verdict::Unknown, extent, "local change at moderate distance");
    }

    case ChangeExtent::Large:
        return make(Verdict::Unknown, extent,
                    "change too large to be a local UI update, too small to be a "
                    "navigation");

    case ChangeExtent::Global:
        // A click CAN legitimately navigate. Whether this navigation was the
        // intended one is a semantic question, which is exactly what the VLM is
        // for.
        return make(Verdict::Unknown, extent,
                    "whole-screen change; navigation needs semantic checking");
    }
    return make(Verdict::Unknown, extent, "unhandled extent");
}

VerificationResult verify_type(ChangeExtent extent)
{
    switch (extent) {
    case ChangeExtent::None:
        // Keystrokes went somewhere that is not on screen: no focused field, a
        // swallowed event, or the wrong window.
        return make(Verdict::Unexpected, extent,
                    "typed text produced no visible change");
    case ChangeExtent::Trace:
    case ChangeExtent::Local:
        // Text entry is a small, contained change. A handful of components is
        // normal -- the field itself, plus a live-filtered list reacting to it.
        return make(Verdict::Expected, extent,
                    "contained change consistent with text entry");
    case ChangeExtent::Large:
    case ChangeExtent::Global:
        // This rule USED to return Unexpected, on the reasoning that typing is
        // a contained act. The corpus refuted it: 6 of 22 type steps changed
        // more than half the frame and every one was correct behaviour --
        // typing "/" opens a rich-text editor's insert picker, and typing into
        // that picker refilters a full-height list (measured: 0.50-0.58 of the
        // frame, 9-10 components).
        //
        // Geometry cannot separate "typing opened a picker" from "typing
        // triggered an accidental navigation", so this is genuinely Unknown.
        // Calling it Unexpected only spends a VLM turn -- the safe error
        // direction -- but it is still a wrong answer, and one that would have
        // made the agent abort six correct action chains in guard mode.
        return make(Verdict::Unknown, extent,
                    "typing caused a screen-wide change; could be a picker or a "
                    "navigation");
    }
    return make(Verdict::Unknown, extent, "unhandled extent");
}

VerificationResult verify_key(ChangeExtent extent, const VerifierConfig& cfg,
                              const VerifierContext& ctx)
{
    // Named keys are the widest-open action there is: Enter submits, Escape
    // closes, Tab moves focus, ctrl+a selects. Judged on its own geometry, every
    // extent is legitimate for some key and nothing can be claimed.
    //
    // But a key does not act in a vacuum: it acts on whatever the last click
    // focused. If the change is small and sits near that inherited focus point,
    // the key did something to the focused control -- which is what a key is
    // supposed to do. Measured on the labelled corpus: 15 of 17 `key` steps were
    // exactly this, mostly arrow-key navigation inside a dropdown a previous
    // click had opened.
    //
    // This is still not a claim about WHICH key did what. A wrong key pressed at
    // a focused control produces the same geometry as the right one. It claims
    // only that the keystroke reached the control it was aimed at, which is the
    // failure the agent actually needs to detect.
    if (extent == ChangeExtent::Trace || extent == ChangeExtent::Local) {
        if (ctx.has_focus && ctx.focus_locality.valid
            && ctx.focus_locality.dist_to_change <= static_cast<double>(cfg.key_focus_radius))
            return make(Verdict::Expected, extent,
                        "contained change at the focused control");
    }
    if (extent == ChangeExtent::None)
        return make(Verdict::Unknown, extent,
                    "key produced no visible change; could be a modifier or a no-op");
    return make(Verdict::Unknown, extent,
                "key outcomes are not distinguishable from geometry alone");
}

VerificationResult verify_scroll(const Action& a, const VisualDelta& d,
                                 const VerifierConfig& cfg, ChangeExtent extent)
{
    // UNVALIDATED: the M0 corpus contains no scrolling whatsoever. These rules
    // follow from what a scroll is, not from measurement, and must be checked
    // against a corpus that actually scrolls before being trusted.
    if (d.transition != TransitionType::Scroll) {
        if (extent == ChangeExtent::None) {
            // A request of exactly zero asks for nothing, so stillness satisfies
            // it. Degenerate, but a rule that calls it a contradiction is wrong.
            if (a.has_dy && a.dy == 0)
                return make(Verdict::Expected, extent,
                            "scroll of 0px requested; the view correctly did not move");
            // Otherwise: already at the end of the document, or nothing
            // scrollable under the pointer.
            return make(Verdict::Unexpected, extent,
                        "scroll requested but the view did not move");
        }
        return make(Verdict::Unknown, extent,
                    "change is not explained by any scroll offset");
    }

    if (!a.has_dy)
        return make(Verdict::Expected, extent,
                    "coherent scroll of " + std::to_string(d.dy) + "px");

    // Sign disagreement is a real contradiction: the view moved the wrong way.
    if ((a.dy > 0) != (d.dy > 0) && a.dy != 0 && d.dy != 0)
        return make(Verdict::Unexpected, extent,
                    "scrolled " + std::to_string(d.dy) + "px against the requested "
                    + std::to_string(a.dy) + "px");

    const double tol = std::max(static_cast<double>(cfg.scroll_tolerance_min_px),
                                std::abs(a.dy) * cfg.scroll_tolerance_frac);
    if (std::abs(d.dy - a.dy) <= tol)
        return make(Verdict::Expected, extent,
                    "scrolled " + std::to_string(d.dy) + "px, matching the request");

    // Short of the request is normal at a document end; the agent still needs to
    // learn it did not get what it asked for.
    return make(Verdict::Unknown, extent,
                "scrolled " + std::to_string(d.dy) + "px against a requested "
                + std::to_string(a.dy) + "px");
}

VerificationResult verify_passive(ActionKind kind, ChangeExtent extent)
{
    // wait / screenshot / mouse_move send no input that should alter state.
    // Stillness is exactly what they should produce.
    if (extent == ChangeExtent::None || extent == ChangeExtent::Trace)
        return make(Verdict::Expected, extent,
                    std::string(action_kind_name(kind)) + " left the screen settled");

    // A `wait` is different from the other two: its entire purpose is to let an
    // asynchronous update land, so a contained change during one is the action
    // working, not a surprise. Measured: 4 of 4 such steps in the labelled
    // corpus. (`mouse_move` with the same geometry is NOT included -- there the
    // labels split 1 expected / 2 ambiguous, and claiming it would cost
    // precision to gain a single step.)
    if (kind == ActionKind::Wait && extent == ChangeExtent::Local)
        return make(Verdict::Expected, extent,
                    "wait let a contained update land, which is what it is for");

    // Anything else is the page acting on its own -- an async render, an
    // animation finishing, a late navigation. Real, and worth a look.
    return make(Verdict::Unknown, extent,
                std::string(action_kind_name(kind))
                + " sent no input, but the screen changed on its own");
}

}  // namespace

VerificationResult verify_action(const Action& action, const VisualDelta& delta,
                                 const ChangeLocality& locality,
                                 const VerifierConfig& cfg,
                                 const VerifierContext& ctx)
{
    const ChangeExtent extent = classify_extent(delta.changed_frac, cfg);

    switch (action.kind) {
    case ActionKind::Click:
    case ActionKind::DoubleClick:
    case ActionKind::RightClick:
        return verify_click(action, delta, locality, cfg, extent);

    case ActionKind::Type:
        return verify_type(extent);

    case ActionKind::Key:
        return verify_key(extent, cfg, ctx);

    case ActionKind::Scroll:
        return verify_scroll(action, delta, cfg, extent);

    case ActionKind::Drag:
        // A drag should displace something. Distinguishing a successful drop
        // from a reverted one needs to know what was dragged, which geometry
        // does not carry.
        if (extent == ChangeExtent::None)
            return make(Verdict::Unexpected, extent, "drag moved nothing");
        return make(Verdict::Unknown, extent,
                    "drag outcomes need object identity, not just geometry");

    case ActionKind::MouseDown:
        // Pressing and holding gives at most a pressed-state or focus-ring
        // repaint. Anything larger means the press started something -- a drag
        // preview, a menu -- which geometry cannot name.
        if (extent == ChangeExtent::None || extent == ChangeExtent::Trace)
            return make(Verdict::Expected, extent,
                        "button press left the screen essentially unchanged");
        return make(Verdict::Unknown, extent,
                    "button press caused a substantial change");

    case ActionKind::MouseUp:
        // Release completes whatever the press began. Every outcome is
        // legitimate, including none, so geometry carries no signal at all.
        return make(Verdict::Unknown, extent,
                    "button release outcomes need to know what the press started");

    case ActionKind::MouseMove:
    case ActionKind::Wait:
    case ActionKind::Screenshot:
        return verify_passive(action.kind, extent);

    case ActionKind::Unknown:
        return make(Verdict::Unknown, extent, "unrecognized action");
    }
    return make(Verdict::Unknown, extent, "unhandled action kind");
}
