#!/usr/bin/env bash
# The perception layer's safety net.
#
# perception/ is inherited code with empirically tuned constants -- thresholds
# chosen because they separate specific recordings in test_data/. There are no
# unit tests for it and there should not be: the meaningful assertion is that
# the whole pipeline still produces the same session report on the same corpus.
#
# So: run a reference build and this build over every recorded session, and
# require the reports to be byte-identical. Any refactor that changes a number
# is either a bug or a deliberate retune that must be argued for.
#
#   scripts/regression.sh                        # vs the upstream Verty build
#   REF=/path/to/other/verty scripts/regression.sh
#
# The corpus is not in git (3.5 GB of video). test_data/session_* are symlinks
# into the original Verty checkout.
set -uo pipefail
cd "$(dirname "$0")/.."

REF="${REF:-../verty/build/verty}"
REF_ROOT="${REF_ROOT:-../verty}"
NEW="./build/verty"
T="${TMPDIR:-/tmp}/verty-regression.$$"

[ -x "$NEW" ] || { echo "build $NEW first"; exit 2; }
[ -x "$REF" ] || { echo "reference build not found: $REF"; exit 2; }

mkdir -p "$T/ref" "$T/new"
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
for d in test_data/session_*; do
  s=$(basename "$d")
  [ -f "$d/session.mp4" ] || continue

  (cd "$REF_ROOT" && "$REF" "test_data/$s/session.mp4" \
      -o "$T/ref/$s.png" -r "$T/ref/$s.json" >/dev/null 2>&1)
  "$NEW" "$d/session.mp4" -o "$T/new/$s.png" -r "$T/new/$s.json" >/dev/null 2>&1

  # Only the recorded input and output PATHS may differ between the two runs.
  norm() { sed -e "s@$T/ref/@PATH/@g" -e "s@$T/new/@PATH/@g" -e "s@//@/@g" "$1"; }

  if [ ! -f "$T/ref/$s.json" ] || [ ! -f "$T/new/$s.json" ]; then
    echo "  MISSING  $s"; fail=$((fail+1)); continue
  fi
  if diff -q <(norm "$T/ref/$s.json") <(norm "$T/new/$s.json") >/dev/null 2>&1; then
    echo "  ok       $s"; pass=$((pass+1))
  else
    echo "  DIFF     $s"; fail=$((fail+1))
    diff <(norm "$T/ref/$s.json") <(norm "$T/new/$s.json") | head -12
  fi
done

echo
echo "identical: $pass   differing: $fail"
[ "$fail" -eq 0 ]
