#!/usr/bin/env python3
"""
Generate test_scroll_10.mp4 and ground_truth_10.png.

Frame layout (642×642):
  Header   : y = 0 .. 64      (65 px, full width)
  Footer   : y = 579 .. 641   (63 px, full width) — synthetic input bar
  Left bar : x = 0 .. 161     (162 px, full height)
  Magenta  : x = 641          (1 px, full height — edge probe)
  Content  : x = 162..640, y = 65..578   (479 × 514 px visible)

Scroll: 8 px/frame × 6 steps = 48 px total.
Content is a single 562-row crop from the source — no tiling, no repeated regions.
"""

import cv2
import numpy as np

SRC      = "/home/schaef/Pictures/Screenshots/Screenshot from 2026-06-29 22-33-11.png"
VID_OUT  = "test_scroll_10.mp4"
GT_OUT   = "ground_truth_10.png"

FRAME_W  = 642
FRAME_H  = 642
LEFT_W   = 162
HEADER_H = 65
FOOTER_H = 63
SCROLL_PX = 8
FPS       = 30
PAUSE     = 15   # still frames before / after scroll
SCROLL_N  = 6    # 6 × 8 px = 48 px total scroll

CONTENT_X = LEFT_W                            # 162
CONTENT_Y = HEADER_H                          # 65
CONTENT_W = FRAME_W - LEFT_W - 1             # 479  (last col is magenta)
CONTENT_H = FRAME_H - HEADER_H - FOOTER_H    # 514

# Total document height needed (no tiling)
DOC_H = CONTENT_H + SCROLL_N * SCROLL_PX     # 514 + 48 = 562

# ── Load source ───────────────────────────────────────────────────────────────
src = cv2.imread(SRC)
assert src is not None, f"Cannot read {SRC}"
src_h, src_w = src.shape[:2]
print(f"Source: {src_w}×{src_h}")

# Source crop positions
CHAT_SRC_X = 305   # x where the chat messages panel starts
CHAT_SRC_Y = 90    # y below the 'Files Department Chat' room header (no duplication)

avail_h = src_h - CHAT_SRC_Y   # 800 px of unique content available
avail_w = src_w - CHAT_SRC_X   # 767 px wide
assert avail_h >= DOC_H, f"Not enough source height: {avail_h} < {DOC_H}"
assert avail_w >= CONTENT_W,  f"Not enough source width:  {avail_w} < {CONTENT_W}"

# ── Scrollable document — single unique crop, no tiling ───────────────────────
doc = src[CHAT_SRC_Y: CHAT_SRC_Y + DOC_H,
          CHAT_SRC_X: CHAT_SRC_X + CONTENT_W].copy()   # 562 × 479, exact fit
print(f"Document: {doc.shape[1]}×{doc.shape[0]}px (no tiling)")

# ── Fixed element patches ─────────────────────────────────────────────────────

# Header: top HEADER_H rows of source, width-scaled to FRAME_W (no height change)
header = cv2.resize(src[:HEADER_H, :], (FRAME_W, HEADER_H),
                    interpolation=cv2.INTER_AREA)

# Left sidebar: left LEFT_W columns of source, height-scaled to FRAME_H
left_bar_frame = cv2.resize(src[:, :LEFT_W], (LEFT_W, FRAME_H),
                            interpolation=cv2.INTER_AREA)

# Footer: synthetic message-input bar (light gray + white input field + send btn)
footer = np.full((FOOTER_H, FRAME_W, 3), (230, 230, 230), dtype=np.uint8)
pad = 10
iw  = FRAME_W - 2 * pad - 60   # input field width
iy  = (FOOTER_H - 36) // 2
cv2.rectangle(footer, (pad, iy), (pad + iw, iy + 36), (255, 255, 255), -1)
cv2.rectangle(footer, (pad, iy), (pad + iw, iy + 36), (195, 195, 195),  1)
# Send button (blue filled circle)
cv2.circle(footer, (FRAME_W - 32, FOOTER_H // 2), 18, (204, 122,   0), -1)
cv2.circle(footer, (FRAME_W - 32, FOOTER_H // 2), 18, (160,  90,   0),  1)
# Attachment icon (grey circle)
cv2.circle(footer, (pad + iw + 30, FOOTER_H // 2), 14, (160, 160, 160), -1)

# ── Frame builder ─────────────────────────────────────────────────────────────
def make_frame(scroll_y: int) -> np.ndarray:
    frame = np.empty((FRAME_H, FRAME_W, 3), dtype=np.uint8)
    frame[:] = 30   # dark safety fill

    frame[:, :LEFT_W] = left_bar_frame                                  # sidebar
    doc_s = doc[scroll_y: scroll_y + CONTENT_H]
    frame[CONTENT_Y: CONTENT_Y + doc_s.shape[0],
          CONTENT_X: CONTENT_X + CONTENT_W] = doc_s                    # content
    frame[:HEADER_H, :]        = header                                 # header
    frame[FRAME_H - FOOTER_H:, :] = footer                             # footer
    frame[:, FRAME_W - 3:]     = (255, 0, 255)                         # 3-px magenta edge
    return frame

# ── Write video ───────────────────────────────────────────────────────────────
fourcc = cv2.VideoWriter_fourcc(*'mp4v')
writer = cv2.VideoWriter(VID_OUT, fourcc, FPS, (FRAME_W, FRAME_H))
assert writer.isOpened(), "VideoWriter failed to open"

for _ in range(PAUSE):
    writer.write(make_frame(0))
for step in range(SCROLL_N):
    writer.write(make_frame((step + 1) * SCROLL_PX))
for _ in range(PAUSE):
    writer.write(make_frame(SCROLL_N * SCROLL_PX))

writer.release()
total = PAUSE + SCROLL_N + PAUSE
print(f"Video:  {VID_OUT}  ({total} frames @ {FPS} fps = {total/FPS:.1f}s)")

# ── Ground truth (8-px-strip reconstruction) ──────────────────────────────────
# Initial visible slice + one new 8-px strip per scroll step.
# All content is drawn from doc[], which has no repeated regions.
strips = [doc[0: CONTENT_H].copy()]
for step in range(SCROLL_N):
    new_y = (step + 1) * SCROLL_PX
    strips.append(doc[new_y + CONTENT_H - SCROLL_PX: new_y + CONTENT_H].copy())

scroll_col = np.concatenate(strips, axis=0)  # 514 + 6*8 = 562 rows × 479 cols
assert scroll_col.shape[0] == DOC_H, f"GT height mismatch: {scroll_col.shape[0]} vs {DOC_H}"
print(f"Scroll column: {scroll_col.shape[1]}×{scroll_col.shape[0]}px (unique)")

gt_h = HEADER_H + scroll_col.shape[0] + FOOTER_H
gt   = np.zeros((gt_h, FRAME_W, 3), dtype=np.uint8)

# Content column
gt[HEADER_H: HEADER_H + scroll_col.shape[0],
   CONTENT_X: CONTENT_X + CONTENT_W] = scroll_col

# Left sidebar: exactly one viewport height (FRAME_H = 642px).
# A fixed sidebar can never be taller than the physical viewport.
# Reuse the same scaled patch used in every video frame so pixels match.
# header/footer strips written below will overwrite their respective slices.
gt[:FRAME_H, :LEFT_W] = left_bar_frame

gt[:HEADER_H, :]       = header          # header
gt[gt_h - FOOTER_H:, :] = footer         # footer
gt[:, FRAME_W - 3:]    = (255, 0, 255)   # 3-px magenta edge

cv2.imwrite(GT_OUT, gt)
print(f"GT:     {GT_OUT}  {FRAME_W}×{gt_h}px")
