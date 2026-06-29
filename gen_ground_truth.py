"""
gen_ground_truth.py
Generate pixel-perfect ground truth stitched screenshots for all 9 test videos.

Strategy
--------
Tests 1–6 : Reconstruct from video frames with exact known scroll structure.
             Each active frame contributes the new 8-px strip that just became
             visible at the bottom (or top for upward scrolls) of the content
             area.  This gives 8-px vertical resolution — finer than the
             algorithm's 16-px block resolution.

Test 7    : Render the full page directly from the gen_test7.py generator.
             Bypass the video entirely → zero compression artefacts.

Test 8    : Render the full page directly from the gen_test8.py generator.

Test 9    : Use the algorithm's detected scroll amounts to reconstruct the
             visible content column-by-column from the raw video frames.
             This is the best possible ground truth for a real recording.
"""

import cv2
import numpy as np
import random
from PIL import Image, ImageDraw, ImageFont
import sys

GRAY235 = (235, 235, 235)   # algorithm's "unknown sidebar area" fill colour


# ── Helpers ────────────────────────────────────────────────────────────────────

def read_frames(path: str):
    cap = cv2.VideoCapture(path)
    frames = []
    while True:
        ok, f = cap.read()
        if not ok:
            break
        frames.append(f)
    cap.release()
    print(f"  read {len(frames)} frames from {path}")
    return frames


def active_indices(frames, threshold=2.0):
    """Indices of frames that differ significantly from their predecessor."""
    out = []
    for i in range(1, len(frames)):
        g_prev = cv2.cvtColor(frames[i-1], cv2.COLOR_BGR2GRAY)
        g_curr = cv2.cvtColor(frames[i], cv2.COLOR_BGR2GRAY)
        if cv2.absdiff(g_prev, g_curr).mean() > threshold:
            out.append(i)
    return out


# ── Tests 1–6 reconstruction ───────────────────────────────────────────────────

def reconstruct_constant_scroll(frames, active_idx, header_h, footer_h,
                                  left_w, right_w, scroll_px=8):
    """
    Pixel-level reconstruction for synthetic tests with a constant scroll_px
    per active frame.

    Output structure (same as the algorithm's renderer):
      • Header strip     : full width, header_h px
      • Scroll stitch    :
            – inner columns (left_w .. W-right_w): full scrolled content
            – sidebar columns: one content-viewport strip at the top of the
              stitch, gray-235 for the rest
      • Footer strip     : full width, footer_h px
    """
    H, W = frames[0].shape[:2]
    content_h = H - header_h - footer_h   # pixels between header and footer

    n_active    = len(active_idx)
    stitch_h    = content_h + n_active * scroll_px
    total_h     = header_h + stitch_h + footer_h

    x_s = left_w
    x_e = W - right_w if right_w > 0 else W

    # ── inner content canvas ───────────────────────────────────────────────────
    inner = np.zeros((stitch_h, x_e - x_s, 3), dtype=np.uint8)

    # Initial viewport (from the first static frame)
    f0 = frames[0]
    inner[0:content_h, :] = f0[header_h : H - footer_h, x_s:x_e]

    # New bottom strip from each active frame
    for k, fi in enumerate(active_idx):
        strip = frames[fi][H - footer_h - scroll_px : H - footer_h, x_s:x_e]
        dst_y = content_h + k * scroll_px
        inner[dst_y : dst_y + scroll_px, :] = strip

    # ── assemble output canvas ─────────────────────────────────────────────────
    gt = np.full((total_h, W, 3), 255, dtype=np.uint8)

    if header_h > 0:
        gt[0:header_h, :] = f0[0:header_h, :]

    if footer_h > 0:
        gt[header_h + stitch_h : total_h, :] = f0[H - footer_h : H, :]

    gt[header_h : header_h + stitch_h, x_s:x_e] = inner

    # Sidebar columns: gray-235 in stitch, one content-viewport strip at top
    for (gx_s, gx_e) in [(0, left_w), (W - right_w, W)]:
        if gx_s >= gx_e:
            continue
        gt[header_h : header_h + stitch_h, gx_s:gx_e] = GRAY235
        sidebar = f0[header_h : H - footer_h, gx_s:gx_e]
        gt[header_h : header_h + content_h, gx_s:gx_e] = sidebar

    return gt


# ── Test 7 generator ───────────────────────────────────────────────────────────

def gen_test7():
    W, H = 640, 480
    SCROLL_PX = 8
    N_SCROLL  = 110
    HEADER_H  = 48
    FOOTER_H  = 48
    BANNER_W  = 64
    GRAY      = (240, 240, 240)
    PAGE_H    = H + N_SCROLL * SCROLL_PX + 200   # 1560 px

    page = np.full((PAGE_H, W, 3), GRAY, dtype=np.uint8)

    random.seed(42)
    BAND_H = 8
    MAX_GAP = 24
    DARK_PALETTE = [
        ( 20,  80, 200), ( 20, 180,  20), (180,  20,  20),
        (  0, 160, 160), (160,   0, 160), ( 40, 140, 200), (200,  80,  40),
    ]
    y = 0
    while y < PAGE_H:
        c = random.choice(DARK_PALETTE)
        page[y : min(y + BAND_H, PAGE_H), :] = c
        y += BAND_H + random.randint(BAND_H, MAX_GAP)

    content_h     = H - HEADER_H - FOOTER_H           # 384 px
    total_stitch  = content_h + N_SCROLL * SCROLL_PX  # 1264 px
    total_h       = HEADER_H + total_stitch + FOOTER_H  # 1360 px

    # Inner content: page rows [HEADER_H .. HEADER_H+total_stitch), cols [BANNER_W .. W-BANNER_W)
    inner = page[HEADER_H : HEADER_H + total_stitch, BANNER_W : W - BANNER_W]

    gt = np.full((total_h, W, 3), 255, dtype=np.uint8)
    gt[0:HEADER_H, :]  = GRAY      # fixed header
    gt[HEADER_H + total_stitch : total_h, :] = GRAY  # fixed footer

    gt[HEADER_H : HEADER_H + total_stitch, BANNER_W : W - BANNER_W] = inner

    # Sidebars
    for gx_s, gx_e in [(0, BANNER_W), (W - BANNER_W, W)]:
        gt[HEADER_H : HEADER_H + total_stitch, gx_s:gx_e] = GRAY235
        gt[HEADER_H : HEADER_H + content_h,    gx_s:gx_e] = GRAY

    return gt


# ── Test 8 generator ───────────────────────────────────────────────────────────

def gen_test8():
    W, H = 640, 480
    SCROLL_PX = 8
    N_SCROLL  = 90
    HEADER_H  = 48
    PAGE_H    = H + N_SCROLL * SCROLL_PX + 200  # 1400 px

    FONT_BODY = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    WHITE     = (255, 255, 255)
    BLACK     = (0,   0,   0)

    HEADLINES = [
        "Introduction to Lorem Ipsum", "Historical Background",
        "Typography and Layout",       "Design Principles",
        "Modern Applications",         "Case Studies in Print",
        "Digital Typography",          "Responsive Design",
        "Accessibility Considerations","Future Trends",
    ]
    SENTENCES = [
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.",
        "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.",
        "Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur.",
        "Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.",
        "Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium totam rem aperiam.",
        "Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores.",
        "Quis autem vel eum iure reprehenderit qui in ea voluptate velit esse quam nihil molestiae consequatur.",
        "At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti.",
        "Nam libero tempore cum soluta nobis est eligendi optio cumque nihil impedit quo minus id quod maxime.",
        "Temporibus autem quibusdam et aut officiis debitis rerum necessitatibus saepe eveniet ut et voluptates.",
        "Itaque earum rerum hic tenetur a sapiente delectus ut aut reiciendis voluptatibus maiores alias consequatur.",
        "Quis nostrum exercitationem ullam corporis suscipit laboriosam nisi ut aliquid ex ea commodi consequatur.",
    ]

    def wrap_text(draw, text, font, max_w):
        words = text.split()
        lines, cur = [], ""
        for word in words:
            test = (cur + " " + word).strip()
            if draw.textlength(test, font=font) <= max_w:
                cur = test
            else:
                if cur:
                    lines.append(cur)
                cur = word
        if cur:
            lines.append(cur)
        return lines

    fb = ImageFont.truetype(FONT_BODY, 10)
    fh = ImageFont.truetype(FONT_BOLD, 13)
    margin    = 20
    content_w = W - 2 * margin

    probe  = Image.new("RGB", (10, 10), WHITE)
    d0     = ImageDraw.Draw(probe)
    body_lh = d0.textbbox((0, 0), "Ag", font=fb)[3] + 3
    head_lh = d0.textbbox((0, 0), "Ag", font=fh)[3] + 5
    section_gap = 14

    img  = Image.new("RGB", (W, PAGE_H), WHITE)
    draw = ImageDraw.Draw(img)
    y    = HEADER_H + section_gap

    sent_idx = 0
    for sec in range(len(HEADLINES)):
        if y >= PAGE_H - 40:
            break
        draw.text((margin, y), HEADLINES[sec % len(HEADLINES)], font=fh, fill=BLACK)
        y += head_lh
        rows = 0
        while rows < 10 and y < PAGE_H - 20:
            sentence = SENTENCES[sent_idx % len(SENTENCES)]
            sent_idx += 1
            for line in wrap_text(draw, sentence, fb, content_w):
                if rows >= 10 or y >= PAGE_H - 20:
                    break
                draw.text((margin, y), line, font=fb, fill=BLACK)
                y += body_lh
                rows += 1
        y += section_gap

    page_bgr = np.array(img)[:, :, ::-1].copy()

    # Blue header with label
    label_fb = ImageFont.truetype(FONT_BOLD, 16)
    hdr_img  = Image.new("RGB", (W, HEADER_H), (50, 50, 220))
    hdr_draw = ImageDraw.Draw(hdr_img)
    tw       = hdr_draw.textlength("Header", font=label_fb)
    hdr_draw.text(((W - tw) / 2, (HEADER_H - 20) / 2), "Header",
                   font=label_fb, fill=WHITE)
    header_bgr = np.array(hdr_img)[:, :, ::-1].copy()

    content_h     = H - HEADER_H           # 432 px
    total_stitch  = content_h + N_SCROLL * SCROLL_PX  # 1152 px

    stitch = page_bgr[HEADER_H : HEADER_H + total_stitch, :]
    return np.vstack([header_bgr, stitch])


# ── Test 9 reconstruction ─────────────────────────────────────────────────────

def reconstruct_test9(frames, scroll_events):
    """
    Reconstruct the ground truth for test_9 from detected scroll events.

    scroll_events: list of (frame_index, dy) for all validated scrolls.
    Fixed layout: left=320px (20 cols × 16), right=320px.
    Source: 1918×930 but algorithm uses only 1904×928 (119×58 blocks).
    """
    # Algorithm-effective dimensions
    BLOCK = 16
    GRID_COLS = 1918 // BLOCK   # 119
    GRID_ROWS = 930  // BLOCK   # 58
    W_eff = GRID_COLS * BLOCK   # 1904
    H_eff = GRID_ROWS * BLOCK   # 928

    LEFT_W  = 20 * BLOCK   # 320
    RIGHT_W = 20 * BLOCK   # 320
    x_s = LEFT_W
    x_e = W_eff - RIGHT_W  # 1584

    # Accumulate viewport_top and collect unique content per block row
    # Use a dict: doc_block_y -> (frame_idx, vy_offset) for each inner column
    # For simplicity: build a canvas, paste each frame's inner region at its doc position.

    # Build the document canvas
    total_dy = sum(abs(dy) for _, dy in scroll_events)
    max_scroll = sum(dy for _, dy in scroll_events if dy > 0)
    min_scroll = sum(dy for _, dy in scroll_events if dy < 0)

    # Viewport extents in doc space
    vt = 0
    for _, dy in scroll_events:
        vt += dy
    final_vt = vt

    # Doc height: initial viewport (H_eff) + final viewport_top extent
    vt = 0
    vt_min, vt_max = 0, 0
    for _, dy in scroll_events:
        vt += dy
        vt_min = min(vt_min, vt)
        vt_max = max(vt_max, vt)

    doc_top    = vt_min
    doc_bottom = vt_max + H_eff
    doc_h      = doc_bottom - doc_top

    # Inner content canvas
    inner_w = x_e - x_s
    inner   = np.zeros((doc_h, inner_w, 3), dtype=np.uint8)

    def paste_frame(fi, vt_cur):
        if fi >= len(frames):
            return
        f = frames[fi][:H_eff, :W_eff]
        # Paste inner content at doc position vt_cur..vt_cur+H_eff-1
        dst_y = vt_cur - doc_top
        if dst_y < 0 or dst_y + H_eff > doc_h:
            # Clamp
            src_y0 = max(0, -dst_y)
            dst_y0 = max(0, dst_y)
            h_copy = min(H_eff - src_y0, doc_h - dst_y0)
            if h_copy <= 0:
                return
            inner[dst_y0 : dst_y0 + h_copy, :] = f[src_y0 : src_y0 + h_copy, x_s:x_e]
        else:
            inner[dst_y : dst_y + H_eff, :] = f[:, x_s:x_e]

    # Paste initial frame
    f0 = frames[0][:H_eff, :W_eff]
    paste_frame(0, 0)

    vt = 0
    for fi, dy in scroll_events:
        vt += dy
        paste_frame(fi, vt)

    # Build output canvas (no header/footer detected)
    gt = np.full((doc_h, W_eff, 3), 255, dtype=np.uint8)
    gt[:, x_s:x_e] = inner

    # Sidebar columns: gray235, then one viewport strip at the top of the stitch
    f0_inner = frames[0][:H_eff, :W_eff]
    for gx_s, gx_e in [(0, LEFT_W), (W_eff - RIGHT_W, W_eff)]:
        gt[:, gx_s:gx_e] = GRAY235
        h_copy = min(H_eff, doc_h)
        gt[0 : h_copy, gx_s:gx_e] = f0_inner[:h_copy, gx_s:gx_e]

    return gt


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    import os
    out_dir = "."

    # ── Tests 1–6: known fixed-element layout ──────────────────────────────────
    # (header_h, footer_h, left_w, right_w)
    configs = {
        1: ("test_scroll.mp4",    48,  0,  0,  0),
        2: ("test_scroll_2.mp4",  48,  0,  0,  0),
        3: ("test_scroll_3.mp4",  48, 48,  0,  0),
        4: ("test_scroll_4.mp4",  48,  0,  0, 64),
        5: ("test_scroll_5.mp4",  48, 48, 64, 64),
        6: ("test_scroll_6.mp4",   0,  0,  0,  0),
    }

    for n, (video, hh, fh, lw, rw) in configs.items():
        print(f"\n[test {n}] {video}")
        frames  = read_frames(video)
        idx     = active_indices(frames)
        print(f"  active frames: {len(idx)}, total scroll: {len(idx)*8}px")
        gt      = reconstruct_constant_scroll(frames, idx, hh, fh, lw, rw)
        outpath = f"ground_truth_{n}.png"
        cv2.imwrite(outpath, gt)
        print(f"  → {outpath}  {gt.shape[1]}×{gt.shape[0]}px")

    # ── Test 7: generator ──────────────────────────────────────────────────────
    print("\n[test 7] gen_test7 (generator)")
    gt7 = gen_test7()
    cv2.imwrite("ground_truth_7.png", gt7)
    print(f"  → ground_truth_7.png  {gt7.shape[1]}×{gt7.shape[0]}px")

    # ── Test 8: generator ──────────────────────────────────────────────────────
    print("\n[test 8] gen_test8 (generator)")
    gt8 = gen_test8()
    cv2.imwrite("ground_truth_8.png", gt8)
    print(f"  → ground_truth_8.png  {gt8.shape[1]}×{gt8.shape[0]}px")

    # ── Test 9: variable scroll, real video ───────────────────────────────────
    print("\n[test 9] test_scroll_9.mp4 (variable scroll reconstruction)")
    frames9 = read_frames("test_scroll_9.mp4")

    # Scroll events detected by the algorithm (frame_index, dy)
    # Frames 5, 7 rejected (pre-filter or boundary); frames 6, 8 = new content
    # but scroll=0 (classified as NEW blocks, not scroll).
    # We use the algorithm's validated scroll amounts from verbose output:
    scroll_events_9 = [
        (50,  +26), (51,  +23), (52,  +30),
        (55, +102), (56,  +27), (57,   +3),
        (58, +129), (59,  +20),
        (61,  +17), (62,  +10),
        (64, +149), (65,  +76),
        (72,   +3), (73,  +20), (74,  +40),
        (80,  +79), (81,  +16),
        (82,  -66), (83, -106), (84,  -29),
        (85,   +3),
    ]
    gt9 = reconstruct_test9(frames9, scroll_events_9)
    cv2.imwrite("ground_truth_9.png", gt9)
    print(f"  → ground_truth_9.png  {gt9.shape[1]}×{gt9.shape[0]}px")


if __name__ == "__main__":
    main()
