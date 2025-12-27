#!/bin/bash

set -eu
set -o pipefail

do_run() {
    echo "===== $@ ====="
    ./docker/exec.sh ./build/X86_MESI_Two_Level/gem5.opt configs/deprecated/example/se.py --cpu-type=DerivO3CPU --ruby \
		     --impFlow=1 \
		     --freeParam=3 \
		     -c /bin/echo --options='Hello, world!' "$@"
}

# --scheme={UnsafeBaseline,SpectreSafeFence,FuturisticSafeFence}
# --spt={0,1}
# --moreTransmitInsts={0,1,2}
# --enableShadowL1={0,1}
# --bottomlessShadowL1={0,1}
# --disableUntaint={0,1}
# --fwdUntaint={0,1}
# --bwdUntaint={0,1}
# --idealUntaint={0,1}

(
    do_run --scheme=UnsafeBaseline --spt=0 --moreTransmitInsts=0 --enableShadowL1=0 --bottomlessShadowL1=0 --disableUntaint=0 --fwdUntaint=0 --bwdUntaint=0 --idealUntaint=0
    do_run --scheme=SpectreSafeFence --spt=0
    do_run --scheme=FuturisticSafeFence --spt=0
    do_run --scheme=FuturisticSafeFence --spt=1 --disableUntaint=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1 --bottomlessShadowL1=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1 --idealUntaint=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1 --idealUntaint=1 --moreTransmitInsts=1
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1 --idealUntaint=1 --moreTransmitInsts=2
    do_run --scheme=FuturisticSafeFence --spt=1 --fwdUntaint=1 --bwdUntaint=1 --enableShadowL1=1 --idealUntaint=1 --moreTransmitInsts=2 --impFlow=0
) | tee tmp.log
grep '^Exiting @ tick ' tmp.log > actual.txt
diff expected.txt actual.txt
echo "SUCCESS" >&2
