#!/usr/bin/env bash
# P0 -- can the model emit more than one action per turn?
#
# Measured on 27 earlier runs: 0 of 360 model turns contained a second tool
# call. That looked like a property of the model. It is not obviously so: the
# upstream system prompt says "You may reason before the function call, but
# never write anything after it", which forbids a second call, while the parser
# accepts many and the documentation advertises them.
#
# So this is an A/B on one prompt line, everything else identical. It decides
# whether the plan-aware work has anything to stand on.
set -uo pipefail
N="${1:-3}"
DEMO="${DEMO:-../Qwen-CUA/demo}"
OUT="${OUT:-$(cd "$(dirname "$0")/.." && pwd)/p0}"
mkdir -p "$OUT"; : > "$OUT/index.txt"
cd "$DEMO"

REG='Register Jordan Lee for the Applied Agents Summit using jordan@example.com. Choose the Developer ticket, select the Browser agents workshop, request a vegetarian meal, accept the code of conduct, submit the form, verify the confirmation, then terminate successfully.'
KAN='Reorganize the Launch Board so Backlog contains "Write release notes", In Progress contains "Polish demo" followed by "Run browser tests", and Done contains "Tag release". Verify the final order, then terminate successfully.'

for i in $(seq 1 "$N"); do
  for batch in false true; do
    for scen in registration-complete kanban-reprioritize; do
      set -a; source .env; set +a
      export QWEN_CUA_ALLOW_BATCH="$batch" QWEN_CUA_VERTY_URL=""
      case "$scen" in
        registration-complete) PROMPT="$REG" ;;
        kanban-reprioritize)   PROMPT="$KAN" ;;
      esac
      echo "  [batch=$batch] $scen run $i"
      ./.venv/bin/qwen-cua run --scenario "$scen" --prompt "$PROMPT" \
        > "$OUT/batch-${batch}_${scen}_${i}.log" 2>&1
      echo "$batch $scen $i $(ls -t data/runs | head -1)" >> "$OUT/index.txt"
    done
  done
done
echo; echo "=== index ==="; cat "$OUT/index.txt"
