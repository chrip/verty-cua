#!/usr/bin/env bash
# Fetch ScreenSpot-v2 (OS-Copilot/ScreenSpot-v2): 1272 grounding samples.
# Annotations are tiny; the image archive is ~1.3 GB.
set -euo pipefail
cd "$(dirname "$0")/data"
BASE="https://huggingface.co/datasets/OS-Copilot/ScreenSpot-v2/resolve/main"

for f in screenspot_desktop_v2.json screenspot_mobile_v2.json screenspot_web_v2.json; do
  [ -f "$f" ] || curl -fL --progress-bar -o "$f" "$BASE/$f"
done

if [ ! -d screenspotv2_image ]; then
  curl -fL --progress-bar -o screenspotv2_image.zip "$BASE/screenspotv2_image.zip"
  unzip -q -o screenspotv2_image.zip
  rm screenspotv2_image.zip
fi

echo "images: $(find screenspotv2_image -name '*.png' | wc -l)"
