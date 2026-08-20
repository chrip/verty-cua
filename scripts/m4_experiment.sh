#!/usr/bin/env bash
# M4: does the visual guard change how the agent performs?
#
# Runs each scenario N times per configuration and writes every run into the
# demo's data/runs, to be tabulated with scripts/compare_runs.py.
#
# Why N and not 1: the model samples at temperature 0.6, so two runs of the same
# task differ substantially on their own. A single guarded run finishing in 11
# turns against an unguarded 22 is not evidence of anything -- measured on the
# first attempt, the guard did not fire once in that run, so the difference was
# entirely sampling. The guard triggers on `unexpected` (~4% of steps in the
# labelled corpus) or a 3-step no-progress run; effects that rare need repetition
# to separate from noise.
#
#   scripts/m4_experiment.sh 5          # 5 runs per config per scenario
set -uo pipefail
N="${1:-5}"
DEMO="${DEMO:-../Qwen-CUA/demo}"
OUT="${OUT:-$(cd "$(dirname "$0")/.." && pwd)/m4}"
mkdir -p "$OUT"
cd "$DEMO"

KANBAN='Reorganize the Launch Board so Backlog contains "Write release notes", In Progress contains "Polish demo" followed by "Run browser tests", and Done contains "Tag release". Verify the final order, then terminate successfully.'
REG='Register Jordan Lee for the Applied Agents Summit using jordan@example.com. Choose the Developer ticket, select the Browser agents workshop, request a vegetarian meal, accept the code of conduct, submit the form, verify the confirmation, then terminate successfully.'

run_one() {  # scenario prompt config index
  local scen="$1" prompt="$2" tag="$3" i="$4"
  set -a; source .env; set +a
  case "$tag" in
    baseline) export QWEN_CUA_VERTY_URL=""            QWEN_CUA_VERTY_FOLD=false ;;
    guard)    export QWEN_CUA_VERTY_URL="$VERTY_URL"  QWEN_CUA_VERTY_FOLD=false ;;
    fold)     export QWEN_CUA_VERTY_URL="$VERTY_URL"  QWEN_CUA_VERTY_FOLD=true  ;;
  esac
  echo "  [$tag] $scen run $i"
  # No `timeout` wrapper: killing the CLI after it finishes emits a spurious
  # run_cancelled that overwrites the run summary. The events remain correct,
  # but there is no reason to corrupt the summary on purpose.
  ./.venv/bin/qwen-cua run --scenario "$scen" --prompt "$prompt" \
    > "$OUT/${tag}_${scen}_${i}.log" 2>&1
  local rid
  rid=$(ls -t data/runs | head -1)
  echo "$tag $scen $i $rid" >> "$OUT/index.txt"
}

: > "$OUT/index.txt"
VERTY_URL="${VERTY_URL:-http://127.0.0.1:4505}"
# SCENARIOS defaults to registration only. Not cherry-picking: a kanban turn
# costs the model up to ~6400 completion tokens of deliberation about the drag,
# which at this decode speed is ~2 minutes per turn and puts a full repeated
# experiment into the hours. Registration exercises clicking, typing and keys,
# completes reliably, and already showed the guard firing 5 times in one run.
# Set SCENARIOS="registration-complete kanban-reprioritize" to include both.
SCENARIOS="${SCENARIOS:-registration-complete}"
for i in $(seq 1 "$N"); do
  for tag in baseline guard fold; do
    for scen in $SCENARIOS; do
      case "$scen" in
        kanban-reprioritize)   run_one "$scen" "$KANBAN" "$tag" "$i" ;;
        registration-complete) run_one "$scen" "$REG"    "$tag" "$i" ;;
      esac
    done
  done
done

echo
echo "=== index ==="
cat "$OUT/index.txt"
echo
echo "Tabulate with:"
echo "  python3 scripts/compare_runs.py $DEMO/data/runs"
