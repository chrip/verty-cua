cmake_minimum_required(VERSION 3.20)
project(verty-cua CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED)

# The include root is `src` and nothing else, so every include names the layer
# it reaches into: "perception/types.hpp", not "types.hpp".
set(VERTY_WARNINGS -Wall -Wextra -O2 -march=native)

# ── perception ───────────────────────────────────────────────────────────────
# The visual engine inherited from Verty, unchanged. test_data/ is its
# regression suite: a change here must leave every session report byte-identical
# unless the change is deliberately about those numbers.
add_library(verty_perception STATIC
    src/perception/scroll_detector.cpp
    src/perception/fixed_accumulator.cpp
    src/perception/page_identifier.cpp
    src/perception/page_registry.cpp
    src/perception/page_stitcher.cpp
    src/perception/scrollbar_map.cpp
    src/perception/scrollbar_tracker.cpp
    src/perception/subframe_stitcher.cpp
    src/perception/subframe_tracker.cpp
    src/perception/transition_classifier.cpp
    src/perception/video_source.cpp
)
target_include_directories(verty_perception PUBLIC src ${OpenCV_INCLUDE_DIRS})
target_link_libraries(verty_perception PUBLIC ${OpenCV_LIBS})
target_compile_options(verty_perception PRIVATE ${VERTY_WARNINGS})

# ── offline ──────────────────────────────────────────────────────────────────
# Video -> transitions -> stitched page -> session report. The original Verty
# pipeline, kept working so fixes can flow both ways with upstream.
add_library(verty_offline STATIC
    src/offline/session_processor.cpp
    src/offline/session_segmenter.cpp
    src/offline/session_report.cpp
)
target_link_libraries(verty_offline PUBLIC verty_perception)
target_compile_options(verty_offline PRIVATE ${VERTY_WARNINGS})

# ── live + verification ──────────────────────────────────────────────────────
# The CUA layer: measure one frame pair cheaply (live/), then decide whether the
# change is what the action should have produced (verification/).
add_library(verty_cua STATIC
    src/live/visual_delta.cpp
    src/live/region_history.cpp
    src/live/block_motion.cpp
    src/live/drag_gesture.cpp
    src/verification/action.cpp
    src/verification/action_verifier.cpp
    src/adapters/trajectory.cpp
)
target_link_libraries(verty_cua PUBLIC verty_perception)
target_compile_options(verty_cua PRIVATE ${VERTY_WARNINGS})

# ── binaries ─────────────────────────────────────────────────────────────────
add_executable(verty src/offline/main.cpp)
target_link_libraries(verty PRIVATE verty_offline)
target_compile_options(verty PRIVATE ${VERTY_WARNINGS})

add_executable(verty-replay src/tools/verty_replay.cpp)
target_link_libraries(verty-replay PRIVATE verty_cua)
target_compile_options(verty-replay PRIVATE ${VERTY_WARNINGS})

# ── verty-serve: M2, the same check as a live loopback service ───────────────
add_executable(verty-serve src/tools/verty_serve.cpp)
target_link_libraries(verty-serve PRIVATE verty_cua)
target_compile_options(verty-serve PRIVATE ${VERTY_WARNINGS})
