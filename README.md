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
