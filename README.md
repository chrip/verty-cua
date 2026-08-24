# Verty-CUA

**Cheap, model-independent visual observability for GUI-agent trajectories.**

A fork of [Verty](https://github.com/chrip/verty), which reconstructs scrolling
pages from screen recordings. Verty-CUA reuses that perception engine to watch
what a computer-use agent is actually doing, from pixels alone — no model, no
GPU, ~9 ms per action.

A benchmark harness tells you one thing at the end of a run: did the task
succeed. It does not tell you *when* the run became unrecoverable, or why. This
does, while the run is still going.

```
        agent
          │
       action
          │
          ▼
   ┌─────────────┐
   │  Verty-CUA  │  transition · history · interference
   └──────┬──────┘
          │
   ┌──────┴────────────────────────┐
   │              │                │
 verdict     new or revisited   change the
                 state          agent didn't cause
```

## The headline

Measured over 50 recorded trajectories (35 successful, 15 failed):

| | |
|---|---|
| failures detected | **13 / 15** |
| false alarms on healthy runs | **0 / 33** |
| median warning before the run ended | **33 actions** |
| median wall-clock recoverable per failed run | **11.8 min of 13.4 — 88 %** |

The signal that does this is **not** action verification. It is the block-level
history: *has the screen returned to a state it has already shown?* In the
clearest case, 47 of 50 actions genuinely succeeded — every per-action verdict
was `expected` — while the agent cycled forever between five states it had
already visited. No per-action check and no "nothing changed" test sees that.

Detection does not require the agent's cooperation. `QWEN_CUA_VERTY_ABORT_RUN=10`
ends a looping run on visual evidence alone:

```
Run aborted: 10 consecutive actions produced no content that had not
             already been on screen.
```

Live, that turned a failure that previously cost 50 turns and ~800 s into one
costing 17 turns and 88 s, with a diagnosis attached.

## What it detects

Three signals, each validated against ground truth rather than judgement:

| signal | question it answers | how it was validated |
|---|---|---|
| **verdict** | did the action do what it should have? | a hand-labelled corpus |
| **novel / revisit** | is this content new, or a state the screen has shown before? | a task where the agent loops on a dead coordinate |
| **external_change** | did something change that the action did not cause? | a lab that logs its own interruptions |

The third matters more than it looks. Screens change for reasons the agent did
not cause -- collaborative edits, notifications, late renders -- and a verifier
that reads any post-action change as evidence about the action will be
confidently wrong exactly there.

The second turned out to be the most useful in practice. An agent that clicks a
control which does nothing keeps producing change -- focus rings, hover states --
so "nothing changed" never fires. "Nothing NEW appeared" does, and a run of it is
an agent going in circles.

## What it does not do

**It does not make an agent faster or more reliable.** Two applications were
tested and largely failed, and they are recorded because the negative results
are load-bearing:

- **Fewer model calls** — structurally impossible where one turn is one call and
  the model emits one action per turn (0 of 536 turns produced a usable plan).
- **Recovering a stuck agent by telling it** — detection works on every task
  tried; recovery does not. In one run the model was told 49 times that it was
  making no progress and clicked the same dead coordinate 50 times.

Folding verified frames out of the visual history does cut prompt tokens ~33 %
per turn, but takes more turns doing it, and the effect is worth little if you
run the model on your own hardware.

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

python3 scripts/lead_time.py RUNS --stuck 10   # how early failures are detected,
                                               # and how often healthy runs trip it
python3 scripts/score_external.py RUN          # interference detection against a
                                               # lab that logs its own interruptions
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
needs object identity that geometry does not carry; and whole-screen navigation
is not separable from a wrong navigation by geometry at all.

The `labs/` in the companion Qwen-CUA fork include two built for measurement
rather than for the agent: one whose targets move while a plan executes, and one
that changes on its own timer and logs when it did -- the second is what makes
`external_change` scoreable instead of merely plausible.

## Relationship to upstream

`upstream` remains the original Verty repository, and the offline pipeline is
kept working and byte-identical so fixes can flow both ways. Verty-CUA adds
layers above it rather than rewriting it.

## License

Apache-2.0, inherited from Verty. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
