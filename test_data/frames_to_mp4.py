#!/usr/bin/env python3
"""
frames_to_mp4.py — assemble a recorded frame series into an .mp4.

Uses cv2.VideoWriter (mp4v) — the same codec the gen_test*.py fixtures use, so
no ffmpeg dependency. Reads fps from the session's ground_truth.json if present.

Usage:
    python3 frames_to_mp4.py [sessionDir] [outFile]
        sessionDir  dir containing frames/ and ground_truth.json
                    (default: ./session_page1)
        outFile     output mp4 (default: <sessionDir>/session.mp4)
"""
import sys, os, json, glob
import cv2

session = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "session_page1")
frames_dir = os.path.join(session, "frames")
out_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(session, "session.mp4")

fps = 30
gt_path = os.path.join(session, "ground_truth.json")
if os.path.exists(gt_path):
    with open(gt_path) as f:
        fps = json.load(f).get("fps", 30)

frames = sorted(glob.glob(os.path.join(frames_dir, "frame_*.jpg")))
if not frames:
    sys.exit(f"No frames found in {frames_dir}")

first = cv2.imread(frames[0])
h, w = first.shape[:2]

writer = cv2.VideoWriter(out_file, cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
if not writer.isOpened():
    sys.exit("VideoWriter failed to open")

for p in frames:
    img = cv2.imread(p)
    if img.shape[:2] != (h, w):
        img = cv2.resize(img, (w, h))
    writer.write(img)
writer.release()

print(f"✔ {len(frames)} frames @ {fps}fps → {out_file}  ({w}×{h}, {len(frames)/fps:.1f}s)")
