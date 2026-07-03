cmake_minimum_required(VERSION 3.20)
project(verty CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED)

add_executable(verty
    src/main.cpp
    src/scroll_detector.cpp
    src/fixed_accumulator.cpp
    src/page_identifier.cpp
    src/page_registry.cpp
    src/page_stitcher.cpp
    src/session_segmenter.cpp
    src/session_processor.cpp
    src/transition_classifier.cpp
    src/video_source.cpp
    src/session_report.cpp
)

target_include_directories(verty PRIVATE src ${OpenCV_INCLUDE_DIRS})
target_link_libraries(verty PRIVATE ${OpenCV_LIBS})
target_compile_options(verty PRIVATE -Wall -Wextra -O2 -march=native)
