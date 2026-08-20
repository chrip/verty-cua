# Verty-CUA

**Efficient visual state tracking for computer-use agents.**

A fork of [Verty](https://github.com/chrip/verty), which reconstructs scrolling
pages from screen recordings. Verty-CUA reuses that perception engine for a
different question:

> After a computer-use agent takes an action, can a cheap visual check decide
> whether the resulting screen is consistent with the expected outcome — and so
> avoid an expensive VLM turn?

Today's CUA agents send a screenshot through a multimodal model after every
action. That round trip dominates latency and cost, and much of it is spent
confirming something entirely predictable happened — or that nothing happened.

```
        agent
          │
       action
          │
          ▼
   ┌─────────────┐
   │  Verty-CUA  │   visual state check, no model   (~9 ms, CPU)
   └──────┬──────┘
          │
 ┌────────┴────────┐
expected      unknown / unexpected
    │                 │
    ▼                 ▼
 continue          VLM turn
```

The goal is not a better VLM. It is fewer and cheaper VLM turns at the same task
success rate.

## What it does

**Measures one frame pair cheaply.** How much changed, where, in how many
disjoint pieces, whether a scroll explains it, and — from a per-block history of
the session — whether each changed block is showing something *new* or returning
to an appearance it has held before.

**Judges the change against the action.** A click should change something near
the pointer; typing should change a text region; a scroll should move the
document by roughly the requested amount. Where the evidence is unambiguous the
verdict is `Expected`; where it contradicts the action, `Unexpected`; otherwise
`Unknown`.

**`Unknown` is the safe default, by design.** A wrong `Expected` makes an agent
proceed on a false premise and fail silently. A wrong `Unknown` costs one VLM
call. The rules only claim `Expected` on unambiguous evidence.

**Never attributes change without positive evidence.** Screens change for
reasons the agent did not cause — collaborative edits from another session,
notifications, overlays, late renders. Post-action change does *not* imply
change caused by the action, and the verifier is built around that.

## Layout

```
src/
  perception/     the Verty engine, unchanged — block matching, scroll
                  detection, fixed/dynamic regions, subframes, stitching
  offline/        the original video → stitched page pipeline   → `verty`
  live/           one frame pair measured cheaply; per-block history;
                  session state carried across steps
  verification/   action + delta → Expected | Unexpected | Unknown
  adapters/       normalized trajectory manifest reader
  tools/          verty_replay.cpp                              → `verty-replay`
                  verty_serve.cpp                               → `verty-serve`
adapters/         Python normalizers, one per trajectory source
benchmarks/       screenspot/ — GUI grounding harness
scripts/          regression, self-tests, scoring, experiment drivers
test_data/        regression corpus for the perception layer (not in git)
```

Perception is treated as a library with a regression suite, not as code to
rewrite. Every change to it must leave `test_data/` reports byte-identical
unless the change is deliberately about those numbers.

## Build

Requires OpenCV 4 and a C++20 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/verty` (offline stitching, unchanged from upstream),
`build/verty-replay` (offline trajectory analysis) and `build/verty-serve`
(the live check).

## Analyze recorded trajectories

```bash
# Normalize a source into manifests. One adapter per source; both write the
# same TSV schema.
python3 adapters/mcp_bridge.py    ../mcp-desktop-bridge/trajectories -o trajectories
python3 adapters/qwen_cua_demo.py ../Qwen-CUA/demo/data/runs        -o trajectories

# Measure every step, and run the expectation model over it.
./build/verty-replay $(find trajectories -name manifest.tsv | sort) \
    -o run.jsonl --verify --change-maps changemaps

python3 analyze_m0.py run.jsonl

# Render before/after/diff sheets so verdicts can be judged by eye. The change
# map is coloured by history: green = never seen before, red = an earlier
# appearance returned.
python3 review_sheet.py run.jsonl -o review/
```

### Validating the rules

Sheets are sorted by hand into `review/{expected,unexpected,unknown}/` according
to what the verdict *should* have been. That labels the truth rather than
agreeing with a suggestion, which is what makes it possible to see both kinds of
error: a false `Expected` (a silent failure) and an over-cautious `Unknown` (a
VLM call that could have been saved).

```bash
python3 scripts/export_labels.py review/ -o labels.json   # freeze the truth
python3 scripts/score.py run.jsonl labels.json            # confusion matrix
python3 scripts/score.py new.jsonl labels.json --diff run.jsonl
```

Freezing labels by *step identity* rather than by predicted verdict is what
turns a rule change from an argument into a measurement: `--diff` shows exactly
which steps moved and whether each move was right.

## Live service

```bash
build/verty-serve -p 4505

POST /transition?session=ID&before=PATH&after=PATH&action=KIND[&x=&y=&dy=&text=]
POST /session/reset?session=ID
GET  /health
```

Frames are passed as **paths**, not bodies: this is a loopback service running
beside the agent, the screenshots are already on disk, and paths keep the wire
format simple enough to need no JSON parser. The socket binds `INADDR_LOOPBACK`
explicitly — **it reads any file it is given and must never be exposed
off-host.**

Sessions carry the block history, the last focus point, and a count of
consecutive steps that introduced no new content.

Responses carry `external_change` alongside the verdict rather than inside it:
"the action did nothing *and* something unrelated arrived" is two facts, and for
GUI testing the second is often the interesting one.

## Checks

```bash
scripts/regression.sh                 # perception vs a reference build, byte-identical
python3 scripts/scroll_selftest.py    # scroll verification against exact ground truth
```

The scroll self-test builds its own ground truth: it crops viewport-sized
windows out of a stitched page at known offsets, so the scroll between
consecutive crops *is* the difference of those offsets. It exists because
recorded corpora rarely contain scrolling, and a sign error in the scroll path
would look exactly like a real finding.

## Status

Working software, not a finished result. The perception layer is inherited and
regression-tested; the verification layer and the live service are new and have
been validated against a hand-labelled corpus that is not distributed with this
repository. Thresholds were fitted on that corpus and should be re-established
on your own data before being trusted in anger.

Known gaps: reflow (a container resizing and re-laying out its contents) has no
model and lands in the residual bucket; `changed_frac` is amplitude-blind, so a
faint wash over a large area scores like a sharp local edit; drag verification
needs object identity that geometry does not carry.

## Relationship to upstream

`upstream` remains the original Verty repository, and the offline pipeline is
kept working and byte-identical so fixes can flow both ways. Verty-CUA adds
layers above it rather than rewriting it.

## License

Apache-2.0, inherited from Verty. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
