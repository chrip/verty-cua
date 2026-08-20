#pragma once
#include <string>
#include <opencv2/core.hpp>

// A CUA action, normalized across sources. The adapters map each source's
// vocabulary onto this; the verifier reasons only about these kinds, so adding
// a trajectory source never reaches the verification rules.
enum class ActionKind {
    Click,        // left_click and friends
    DoubleClick,
    RightClick,
    Type,         // text entry
    Key,          // named keys / chords: Enter, Escape, ctrl+a
    Scroll,
    Drag,
    MouseDown,    // button pressed and held -- half of a hand-rolled drag
    MouseUp,      // button released -- completes whatever MouseDown started
    MouseMove,    // pointer moves without a button; changes hover state at most
    Wait,         // no input at all -- any change is the page's own doing
    Screenshot,   // observation only
    Unknown,
};

struct Action {
    ActionKind  kind = ActionKind::Unknown;
    bool        has_coord = false;
    cv::Point   coord;
    bool        has_dy = false;
    int         dy = 0;        // requested scroll, in Verty's sign convention
    std::string text;
};

ActionKind action_kind_from_string(const std::string& s);
const char* action_kind_name(ActionKind k);
