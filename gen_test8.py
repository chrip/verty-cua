"""
test_scroll_8.mp4 — realistic text-based web page.

Content: Lorem ipsum paragraphs with a bold headline every 10 body-text rows.
Font:    DejaVu Sans 10px body / 13px bold headline.
BG:      white page, black text.
Fixed:   48px blue header (3 block rows, same wireframe as test_scroll_1).
"""

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import os

# ── params ────────────────────────────────────────────────────────────────────
W, H      = 640, 480
SCROLL_PX = 8
N_STATIC  = 11
N_SCROLL  = 90
FPS       = 30
HEADER_H  = 48      # 3 × BLOCK_SIZE — blue fixed bar

PAGE_H = H + N_SCROLL * SCROLL_PX + 200   # 480+720+200 = 1400 px

FONT_BODY = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

WHITE = (255, 255, 255)
BLACK = (0,   0,   0)

# ── content ───────────────────────────────────────────────────────────────────
HEADLINES = [
    "Introduction to Lorem Ipsum",
    "Historical Background",
    "Typography and Layout",
    "Design Principles",
    "Modern Applications",
    "Case Studies in Print",
    "Digital Typography",
    "Responsive Design",
    "Accessibility Considerations",
    "Future Trends",
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
    """Greedy word-wrap using exact pixel widths."""
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


def render_page():
    """Return a PIL Image of the full page (white bg, black text)."""
    fb = ImageFont.truetype(FONT_BODY, 10)
    fh = ImageFont.truetype(FONT_BOLD, 13)

    margin = 20
    content_w = W - 2 * margin

    # Measure line heights from a scratch canvas
    probe = Image.new("RGB", (10, 10), WHITE)
    d0 = ImageDraw.Draw(probe)
    body_lh = d0.textbbox((0, 0), "Ag", font=fb)[3] + 3   # ascent + descent + leading
    head_lh = d0.textbbox((0, 0), "Ag", font=fh)[3] + 5

    section_gap = 14

    img  = Image.new("RGB", (W, PAGE_H), WHITE)
    draw = ImageDraw.Draw(img)

    y = HEADER_H + section_gap

    sent_idx = 0
    for sec in range(len(HEADLINES)):
        if y >= PAGE_H - 40:
            break
        # Headline
        draw.text((margin, y), HEADLINES[sec % len(HEADLINES)], font=fh, fill=BLACK)
        y += head_lh

        # 10 rows of body text
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

    return img


# ── build page array (BGR) ────────────────────────────────────────────────────
page_pil = render_page()
page_bgr = np.array(page_pil)[:, :, ::-1].copy()   # RGB → BGR, contiguous

# ── make header overlay ───────────────────────────────────────────────────────
header = np.full((HEADER_H, W, 3), (220, 50, 50), dtype=np.uint8)    # BGR blue

# Add "Header" label in the center
label_fb = ImageFont.truetype(FONT_BOLD, 16)
hdr_img = Image.new("RGB", (W, HEADER_H), (50, 50, 220))             # RGB blue
hdr_draw = ImageDraw.Draw(hdr_img)
tw = hdr_draw.textlength("Header", font=label_fb)
hdr_draw.text(((W - tw) / 2, (HEADER_H - 20) / 2), "Header",
              font=label_fb, fill=WHITE)
header = np.array(hdr_img)[:, :, ::-1].copy()

# ── write video ───────────────────────────────────────────────────────────────
out = cv2.VideoWriter(
    "test_scroll_8.mp4",
    cv2.VideoWriter_fourcc(*"mp4v"),
    FPS, (W, H)
)

def make_frame(scroll_y):
    frame = page_bgr[scroll_y : scroll_y + H, 0:W].copy()
    frame[0:HEADER_H, :] = header
    return frame

scroll_y = 0
for _ in range(N_STATIC):
    out.write(make_frame(scroll_y))
for _ in range(N_SCROLL):
    scroll_y += SCROLL_PX
    out.write(make_frame(scroll_y))

out.release()
print(f"Written test_scroll_8.mp4 — {N_STATIC + N_SCROLL} frames, "
      f"scroll {scroll_y}px, page {W}×{PAGE_H}px")
