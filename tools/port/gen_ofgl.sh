#!/usr/bin/env bash
# gen_ofgl.sh: how src/host/ofgl.c was made from MVEX-OpticalFLow's mvexgl.c (commit 7fad41a, kept here as mvexgl.c.orig): the link to the card replaced by the engine, rates and holds; every plane of a dmabuf; --trace; the
# pace. Each edit is anchored on one exact line and a missing anchor fails the run. Prints the file; `gen_ofgl.sh | cmp - ../../src/host/ofgl.c` says whether the source still is what these make.
set -euo pipefail
d=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
awk -v top="$d/ofgl_top.txt" -v mainf="$d/ofgl_main.txt" -v want=29 -f "$d/ofgl.awk" "$d/mvexgl.c.orig" | awk -f "$d/planes.awk" | awk -f "$d/trace.awk" | awk -f "$d/pace.awk" |
    sed -e 's/card_sum/flow_sum/g; s/card_n\b/flow_n/g; s/card_max/flow_max/g' \
        -e "s/The card's vectors/The flow's vectors/; s/to the card, which must see every frame/to the flow, which must see every frame/; s/A frame the card has held too long/A frame the flow has held too long/; s/its luma, the card\./its luma, the flow./" \
        -e 's|^ \* Run through a cast of the monitor: desk cast --output NAME --no-cursor -- out/ofgl --node @NODE@ --output NAME   (scripts/ofgl.sh does it) \*/$| * The cast is asked of KWin by this program itself (zkde_screencast_unstable_v1, src/host/wlp.c); --node names one made some other way. scripts/ofgl.sh starts it; scripts/authorize.sh gives it the right, once. */|'
