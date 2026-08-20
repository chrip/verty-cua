# ScreenSpot-v2 — GUI grounding baseline

The first benchmark in the Verty-CUA plan, and the gate on everything after it.

## Why this benchmark

Verty-CUA's whole premise is that a cheap visual layer can decide when an
expensive VLM turn is unnecessary. That premise assumes the VLM can act
competently in the first place. If the model cannot point at the right pixel,
there is no agent loop to make efficient, and the rest of the roadmap is built
on sand.

ScreenSpot-v2 isolates exactly that capability — screenshot + instruction →
coordinate — in 1,272 samples across desktop, mobile, and web. It needs no VM,
no browser, and no desktop, which is what makes it the one published-number
benchmark that runs on this aarch64 host. OSWorld and WebArena both ship
x86-64-only images and are out of reach locally.

## What it measures that a stock harness does not

The harness speaks the **Qwen-CUA demo protocol verbatim** — the same system
prompt, the same `<tool_call><function=computer_use>` XML dialect, the same
`0..999` relative coordinate grid. So the score predicts how the model will
behave inside the agent runtime, not how it behaves on an idealized prompt.

Because the local model (`unsloth/Qwen3.8-27B-NVFP4`) is **not CUA-tuned**, a low
score is ambiguous on its own. Three metrics separate the causes:

| Metric | Meaning |
|---|---|
| `format_rate` | emitted a well-formed `computer_use` tool call |
| `strict_acc`  | well-formed **and** the point lands inside the ground-truth box |
| `lenient_acc` | any extractable coordinate lands inside the box |

`lenient_acc - strict_acc` is the cost of protocol non-compliance.
`--prompt-mode plain` runs the control: plain-text request for absolute pixel
coordinates, no protocol at all. The gap between `plain` and `cua` is what the
XML dialect and the normalized grid cost this model.

## Setup

```bash
./download_data.sh     # ~1.3 GB of images + 3 annotation files
```

## Run

```bash
# Smoke test first — 30 samples, confirms endpoint, parsing, and scaling.
python3 run_screenspot.py --limit 30

# Full run in the agent protocol.
python3 run_screenspot.py --model qwen3.8-27b --prompt-mode cua

# The control.
python3 run_screenspot.py --model qwen3.8-27b --prompt-mode plain
```

Useful flags:

- `--concurrency 6` — keep at or below the server's `--max-num-seqs` (8 here).
- `--reasoning-effort low` — sent as `chat_template_kwargs.reasoning_effort`.
  Empty string omits it and lets the server default apply.
- `--max-pixels 1600000` — downscale large web screenshots. Ground truth stays
  in original pixels; predictions are mapped back before hit-testing.
- `--resume` — a full run takes a while; partial results are appended to
  `results.jsonl` and can be continued.

## Output

`results/<model>_<mode>/`

- `results.jsonl` — one row per sample: raw response, parsed point, hit,
  latency, token counts. Keeps the raw text so failures can be read rather than
  guessed at.
- `summary.json` — accuracy overall, by split, by split × `text`/`icon`, and by
  data source, plus the exact config used.

The conventional ScreenSpot reporting split is split × `text`/`icon`, which is
what `by_split_type` holds.

## Notes on comparability

Published ScreenSpot-v2 numbers come from harnesses that use each model's own
native grounding prompt, usually with no agent protocol wrapped around it. The
`plain` mode here is the closer analogue; the `cua` mode is deliberately harder
and is the number that matters for this project.
