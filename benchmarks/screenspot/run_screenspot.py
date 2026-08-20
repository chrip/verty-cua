#!/usr/bin/env python3
"""ScreenSpot-v2 grounding benchmark against an OpenAI-compatible endpoint.

The point of this harness is not to produce a leaderboard number in the abstract.
It measures grounding *in the protocol Verty-CUA will actually run*: the same
system prompt, the same <tool_call><function=computer_use> XML dialect, and the
same 0..999 normalized coordinate grid the Qwen-CUA demo uses.

That distinction matters for a model that was never CUA-tuned. A low score can
mean two very different things -- the model cannot point at the right pixel, or
it cannot speak the protocol -- so the two are measured separately:

    format_rate   emitted a well-formed computer_use tool call
    strict_acc    well-formed AND the point lands inside the ground-truth box
    lenient_acc   any extractable coordinate lands inside the box

Run `--prompt-mode plain` for the control: ask for absolute pixel coordinates in
plain text, no protocol at all. The gap between plain and cua isolates protocol
cost from grounding ability.
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, asdict
from pathlib import Path

import requests
from PIL import Image

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
SPLITS = ("desktop", "mobile", "web")

# ── The Qwen-CUA protocol, mirrored from Qwen-CUA/demo/src/qwen_cua/protocol.py ──
# Kept byte-compatible on purpose: if the demo's prompt changes, this must change
# with it, or the benchmark stops predicting how the agent will behave.

ACTIONS = [
    "key", "key_down", "key_up", "left_mouse_down", "left_mouse_up", "type",
    "mouse_move", "left_click", "left_click_drag", "right_click", "middle_click",
    "double_click", "triple_click", "scroll", "hscroll", "screenshot", "wait",
    "terminate", "call_user",
]


def build_tool_definition() -> dict:
    return {
        "type": "function",
        "function": {
            "name": "computer_use",
            "description": "\n".join([
                "Use a mouse and keyboard to interact with a browser.",
                "* The coordinate system is a 1000x1000 relative grid.",
                "* Inspect each screenshot before choosing coordinates.",
                "* Use screenshot or wait when the interface needs time to update.",
                "* Verify the visible result before terminating.",
            ]),
            "parameters": {
                "type": "object",
                "required": ["action"],
                "properties": {
                    "action": {"type": "string", "enum": ACTIONS},
                    "keys": {"type": "array", "items": {"type": "string"}},
                    "text": {"type": "string"},
                    "coordinate": {
                        "type": "array", "items": {"type": "number"},
                        "minItems": 2, "maxItems": 2,
                    },
                    "pixels": {"type": "number"},
                    "time": {"type": "number"},
                    "duration": {"type": "number"},
                    "status": {"type": "string", "enum": ["success", "failure"]},
                },
            },
        },
    }


CUA_SYSTEM = (
    "You are a multi-purpose intelligent assistant. Based on the user's request, "
    "use the computer tool to complete browser tasks.\n\n"
    "# Tools\n\n"
    "You have access to the following functions:\n\n"
    f"<tools>\n{json.dumps(build_tool_definition(), ensure_ascii=False)}\n</tools>\n\n"
    "If you choose to call a function, reply in this exact format with no suffix:\n\n"
    "<tool_call>\n<function=computer_use>\n<parameter=action>\nleft_click\n</parameter>\n"
    "<parameter=coordinate>\n[500, 500]\n</parameter>\n</function>\n</tool_call>\n\n"
    "<IMPORTANT>\n"
    "- Function calls must contain an inner <function=computer_use> block inside <tool_call> tags.\n"
    "- Include every parameter required by the chosen action.\n"
    "- You may reason before the function call, but never write anything after it.\n"
    "- Interact through the visible GUI only.\n"
    "</IMPORTANT>"
)

CUA_USER = (
    "Here is a screenshot of the screen. Click the element that matches this "
    "instruction:\n\n{instruction}\n\n"
    "Reply with exactly one left_click tool call giving the coordinate on the "
    "1000x1000 relative grid. Do not do anything else."
)

PLAIN_SYSTEM = (
    "You locate elements in screenshots. You answer with a single pixel coordinate "
    "and nothing else."
)

PLAIN_USER = (
    "The screenshot is {w} pixels wide and {h} pixels tall.\n\n"
    "Find the element matching this instruction:\n\n{instruction}\n\n"
    "Reply with only the absolute pixel coordinate of its center, formatted "
    "exactly as [x, y]. No explanation."
)

# ── Response parsing ────────────────────────────────────────────────────────
TOOL_CALL_RE = re.compile(r"<tool_call>(.*?)</tool_call>", re.DOTALL | re.IGNORECASE)
FUNCTION_RE = re.compile(r"<function=([^>]+)>", re.IGNORECASE)
PARAM_RE = re.compile(r"<parameter=([^>]+)>(.*?)</parameter>", re.DOTALL | re.IGNORECASE)
PAIR_RE = re.compile(r"[\[(]\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*[\])]")
BARE_PAIR_RE = re.compile(r"(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)")


def parse_cua(response: str):
    """Return (point, how). `how` is 'protocol' for a well-formed tool call."""
    for block in TOOL_CALL_RE.findall(response):
        fn = FUNCTION_RE.search(block)
        if not fn or fn.group(1).strip() != "computer_use":
            continue
        params = {n.strip(): v.strip() for n, v in PARAM_RE.findall(block)}
        raw = params.get("coordinate")
        if not raw:
            continue
        m = PAIR_RE.search(raw) or BARE_PAIR_RE.search(raw)
        if m:
            return (float(m.group(1)), float(m.group(2))), "protocol"
    m = PAIR_RE.search(response) or BARE_PAIR_RE.search(response)
    if m:
        return (float(m.group(1)), float(m.group(2))), "loose"
    return None, "none"


def parse_plain(response: str):
    m = PAIR_RE.search(response) or BARE_PAIR_RE.search(response)
    if m:
        return (float(m.group(1)), float(m.group(2))), "protocol"
    return None, "none"


# ── Sample handling ─────────────────────────────────────────────────────────
@dataclass
class Result:
    idx: int
    split: str
    img: str
    instruction: str
    data_type: str
    data_source: str
    bbox: list          # [x, y, w, h] absolute pixels
    img_w: int          # size the model saw (after optional downscale)
    img_h: int
    orig_w: int         # ground-truth pixel space
    orig_h: int
    raw: str
    point: list | None  # absolute pixels, after un-normalizing
    parse: str
    hit: bool
    latency_s: float
    prompt_tokens: int
    completion_tokens: int
    error: str | None = None


def load_samples(splits, limit=None):
    out = []
    for split in splits:
        rows = json.loads((DATA / f"screenspot_{split}_v2.json").read_text())
        for r in rows:
            r = dict(r)
            r["split"] = split
            out.append(r)
    out.sort(key=lambda r: (r["split"], r["img_filename"], r["instruction"]))
    for i, r in enumerate(out):
        r["idx"] = i
    return out[:limit] if limit else out


def encode_image(path: Path, max_pixels: int | None):
    im = Image.open(path).convert("RGB")
    ow, oh = im.size
    if max_pixels and ow * oh > max_pixels:
        scale = (max_pixels / (ow * oh)) ** 0.5
        im = im.resize((max(1, int(ow * scale)), max(1, int(oh * scale))), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "PNG")
    w, h = im.size
    return base64.b64encode(buf.getvalue()).decode(), w, h, ow, oh


def build_messages(mode, b64, instruction, w, h):
    img = {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b64}"}}
    if mode == "cua":
        return [
            {"role": "system", "content": CUA_SYSTEM},
            {"role": "user", "content": [img, {"type": "text",
             "text": CUA_USER.format(instruction=instruction)}]},
        ]
    return [
        {"role": "system", "content": PLAIN_SYSTEM},
        {"role": "user", "content": [img, {"type": "text",
         "text": PLAIN_USER.format(instruction=instruction, w=w, h=h)}]},
    ]


def run_one(cfg, sample) -> Result:
    img_dir = DATA / "screenspotv2_image"
    path = img_dir / sample["img_filename"]
    b64, w, h, ow, oh = encode_image(path, cfg.max_pixels)
    messages = build_messages(cfg.prompt_mode, b64, sample["instruction"], w, h)

    body = {
        "model": cfg.model,
        "messages": messages,
        "max_tokens": cfg.max_tokens,
        "temperature": cfg.temperature,
        "top_p": cfg.top_p,
    }
    extra = {}
    if cfg.reasoning_effort:
        extra["reasoning_effort"] = cfg.reasoning_effort
    if extra:
        body["chat_template_kwargs"] = extra

    base = Result(
        idx=sample["idx"], split=sample["split"], img=sample["img_filename"],
        instruction=sample["instruction"], data_type=sample["data_type"],
        data_source=sample["data_source"], bbox=sample["bbox"], img_w=w, img_h=h,
        orig_w=ow, orig_h=oh,
        raw="", point=None, parse="none", hit=False, latency_s=0.0,
        prompt_tokens=0, completion_tokens=0,
    )

    t0 = time.time()
    try:
        resp = requests.post(
            f"{cfg.base_url}/chat/completions", json=body,
            headers={"Authorization": f"Bearer {cfg.api_key}"}, timeout=cfg.timeout,
        )
        resp.raise_for_status()
        payload = resp.json()
    except Exception as exc:
        base.latency_s = round(time.time() - t0, 2)
        base.error = f"{type(exc).__name__}: {exc}"[:300]
        return base

    base.latency_s = round(time.time() - t0, 2)
    msg = payload["choices"][0]["message"]
    # vLLM 0.23 with --reasoning-parser=qwen3 splits thinking into `reasoning`;
    # older builds and other servers use `reasoning_content`. Content is what we
    # parse, but keep reasoning as a fallback so a truncated turn is still visible.
    content = msg.get("content") or ""
    reasoning = msg.get("reasoning") or msg.get("reasoning_content") or ""
    base.raw = content if content.strip() else reasoning
    usage = payload.get("usage") or {}
    base.prompt_tokens = usage.get("prompt_tokens", 0)
    base.completion_tokens = usage.get("completion_tokens", 0)
    if payload["choices"][0].get("finish_reason") == "length" and not content.strip():
        base.error = "truncated_in_reasoning"

    point, how = (parse_cua if cfg.prompt_mode == "cua" else parse_plain)(base.raw)
    base.parse = how
    if point is None:
        return base

    # Ground truth is in ORIGINAL pixels, so every prediction is mapped back to
    # original pixel space before hit-testing. Downscaling is uniform, so the
    # relative grid needs no correction; plain mode does, because the model was
    # told the downscaled size.
    x, y = point
    if cfg.prompt_mode == "cua":
        px, py = x / 1000.0 * ow, y / 1000.0 * oh
    else:
        px, py = x * (ow / w), y * (oh / h)

    base.point = [round(px, 1), round(py, 1)]
    bx, by, bw, bh = sample["bbox"]
    base.hit = (bx <= px <= bx + bw) and (by <= py <= by + bh)
    return base


def summarize(results):
    def acc(rows, strict=True):
        if not rows:
            return None
        ok = [r for r in rows if r["hit"] and (r["parse"] == "protocol" or not strict)]
        return round(100.0 * len(ok) / len(rows), 1)

    def fmt(rows):
        if not rows:
            return None
        return round(100.0 * sum(1 for r in rows if r["parse"] == "protocol") / len(rows), 1)

    out = {"n": len(results), "overall": {
        "strict_acc": acc(results), "lenient_acc": acc(results, strict=False),
        "format_rate": fmt(results),
        "errors": sum(1 for r in results if r.get("error")),
        "mean_latency_s": round(sum(r["latency_s"] for r in results) / max(1, len(results)), 2),
        "mean_prompt_tokens": round(sum(r["prompt_tokens"] for r in results) / max(1, len(results))),
        "mean_completion_tokens": round(sum(r["completion_tokens"] for r in results) / max(1, len(results))),
    }, "by_split": {}, "by_split_type": {}, "by_source": {}}

    for split in sorted({r["split"] for r in results}):
        rows = [r for r in results if r["split"] == split]
        out["by_split"][split] = {"n": len(rows), "strict_acc": acc(rows),
                                  "lenient_acc": acc(rows, strict=False), "format_rate": fmt(rows)}
        for dt in ("text", "icon"):
            sub = [r for r in rows if r["data_type"] == dt]
            out["by_split_type"][f"{split}_{dt}"] = {"n": len(sub), "strict_acc": acc(sub)}
    for src in sorted({r["data_source"] for r in results}):
        rows = [r for r in results if r["data_source"] == src]
        out["by_source"][src] = {"n": len(rows), "strict_acc": acc(rows)}
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base-url", default="http://127.0.0.1:8000/v1")
    p.add_argument("--api-key", default="dummy")
    p.add_argument("--model", default="qwen3.8-27b")
    p.add_argument("--prompt-mode", choices=["cua", "plain"], default="cua")
    p.add_argument("--splits", default="desktop,mobile,web")
    p.add_argument("--limit", type=int, default=None, help="first N samples (smoke test)")
    p.add_argument("--concurrency", type=int, default=6, help="keep <= vLLM --max-num-seqs")
    p.add_argument("--max-tokens", type=int, default=2048)
    p.add_argument("--temperature", type=float, default=0.0)
    p.add_argument("--top-p", type=float, default=1.0)
    p.add_argument("--reasoning-effort", default="low",
                   help="chat_template_kwargs.reasoning_effort; '' to omit")
    p.add_argument("--max-pixels", type=int, default=None,
                   help="downscale images above this pixel count (e.g. 1600000)")
    p.add_argument("--timeout", type=float, default=300.0)
    p.add_argument("--out", default=None, help="results dir (default results/<tag>)")
    p.add_argument("--resume", action="store_true")
    cfg = p.parse_args()
    if cfg.reasoning_effort == "":
        cfg.reasoning_effort = None

    splits = [s.strip() for s in cfg.splits.split(",") if s.strip()]
    samples = load_samples(splits, cfg.limit)

    img_dir = DATA / "screenspotv2_image"
    if not img_dir.is_dir():
        sys.exit(f"images not found at {img_dir} -- run download_data.sh first")

    tag = cfg.out or f"{cfg.model.replace('/', '_')}_{cfg.prompt_mode}"
    outdir = HERE / "results" / tag
    outdir.mkdir(parents=True, exist_ok=True)
    jsonl = outdir / "results.jsonl"

    done = {}
    if cfg.resume and jsonl.exists():
        for line in jsonl.read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                done[r["idx"]] = r
        print(f"resuming: {len(done)} samples already done")

    todo = [s for s in samples if s["idx"] not in done]
    print(f"model={cfg.model} mode={cfg.prompt_mode} effort={cfg.reasoning_effort} "
          f"samples={len(samples)} todo={len(todo)} concurrency={cfg.concurrency}")

    lock = threading.Lock()
    fh = jsonl.open("a")
    counter = {"n": 0, "hit": 0, "t0": time.time()}

    def work(sample):
        r = run_one(cfg, sample)
        row = asdict(r)
        with lock:
            fh.write(json.dumps(row) + "\n")
            fh.flush()
            done[r.idx] = row
            counter["n"] += 1
            counter["hit"] += int(r.hit)
            n = counter["n"]
            if n % 20 == 0 or n == len(todo):
                el = time.time() - counter["t0"]
                rate = n / el if el else 0
                eta = (len(todo) - n) / rate if rate else 0
                print(f"  {n}/{len(todo)}  acc={100*counter['hit']/n:.1f}%  "
                      f"{rate:.2f}/s  eta={eta/60:.1f}m", flush=True)
        return row

    try:
        with ThreadPoolExecutor(max_workers=cfg.concurrency) as pool:
            list(pool.map(work, todo))
    except KeyboardInterrupt:
        print("\ninterrupted -- partial results kept; rerun with --resume")
    finally:
        fh.close()

    results = [done[s["idx"]] for s in samples if s["idx"] in done]
    summary = summarize(results)
    summary["config"] = {k: v for k, v in vars(cfg).items() if k != "api_key"}
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2))

    o = summary["overall"]
    print(f"\n=== {tag} ===")
    print(f"n={summary['n']}  strict={o['strict_acc']}%  lenient={o['lenient_acc']}%  "
          f"format={o['format_rate']}%  errors={o['errors']}")
    print(f"latency={o['mean_latency_s']}s  prompt_tok={o['mean_prompt_tokens']}  "
          f"completion_tok={o['mean_completion_tokens']}")
    for split, v in summary["by_split"].items():
        t = summary["by_split_type"].get(f"{split}_text", {}).get("strict_acc")
        i = summary["by_split_type"].get(f"{split}_icon", {}).get("strict_acc")
        print(f"  {split:8s} n={v['n']:4d}  strict={v['strict_acc']}%  "
              f"(text {t}% / icon {i}%)  format={v['format_rate']}%")
    print(f"\nwrote {outdir}/summary.json")


if __name__ == "__main__":
    main()
