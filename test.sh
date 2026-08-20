#!/bin/bash

set -eu
set -o pipefail

do_run() {
    ./docker/exec.sh ./build/X86/gem5.opt configs/deprecated/example/se.py --caches --cpu-type=X86O3CPU -c /bin/echo --options='Hello, world!' "$@"
}

for threat_model in UnsafeBaseline Spectre Futuristic; do
    for STT in 0 1; do
	if [[ "$threat_model" = "UnsafeBaseline" && "$STT" = "1" ]]; then
	    continue
	fi
	for implicit_channel in 0 1; do
	    if [[ "$STT" = "0" && "$implicit_channel" = "1" ]]; then
		continue
	    fi
	    do_run --STT=$STT --threat_model=$threat_model --implicit_channel=$implicit_channel
	done
    done
done | tee tmp.log
grep '^Exiting @ tick ' tmp.log > actual.txt
diff expected.txt actual.txt
echo "SUCCESS" >&2
