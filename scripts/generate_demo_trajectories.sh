#!/usr/bin/env bash
# Generate CUA trajectories from the Qwen-CUA demo labs, then normalize them.
#
# The bridge corpus that M0/M1 were built on has no scrolling, no drags, and one
# application family. These two labs cover what it misses: `kanban` is
# drag-and-drop, `registration` is a multi-step form with typing and scrolling.
# Both verify deterministically against explicit lab state, so a trajectory
# comes with ground truth about whether the task actually succeeded.
set -euo pipefail

DEMO="${DEMO:-../Qwen-CUA/demo}"
OUT="${OUT:-$(cd "$(dirname "$0")/.." && pwd)/trajectories}"
REPEATS="${REPEATS:-2}"

cd "$DEMO"
set -a; source .env; set +a

for i in $(seq 1 "$REPEATS"); do
  echo "=== round $i/$REPEATS ==="

  ./.venv/bin/qwen-cua run \
    --scenario kanban-reprioritize \
    --prompt "Reorganize the Launch Board so Backlog contains \"Write release notes\", In Progress contains \"Polish demo\" followed by \"Run browser tests\", and Done contains \"Tag release\". Verify the final order, then terminate successfully." \
    || echo "  (kanban run $i ended non-zero -- partial trajectory is still usable)"

  ./.venv/bin/qwen-cua run \
    --scenario registration-complete \
    --prompt "Register Jordan Lee for the Applied Agents Summit using jordan@example.com. Choose the Developer ticket, select the Browser agents workshop, request a vegetarian meal, accept the code of conduct, submit the form, verify the confirmation, then terminate successfully." \
    || echo "  (registration run $i ended non-zero -- partial trajectory is still usable)"
done

echo
echo "=== normalizing ==="
cd "$(dirname "$0")/.."
python3 adapters/qwen_cua_demo.py "$DEMO/data/runs" -o "$OUT"

echo
echo "=== analyzing ==="
./build/verty-replay $(find "$OUT" -name manifest.tsv | sort) -o m0_all.jsonl --verify
python3 analyze_m0.py m0_all.jsonl
