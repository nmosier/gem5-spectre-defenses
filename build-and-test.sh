#!/bin/bash

set -eu

truncate -s 0 build.out
tail -f build.out &
tail_pid=$!
trap "kill $tail_pid" EXIT
sbatch --account=cafe --partition=cafe --wait -c 48 --mem=128G -o build.out --wrap=./build.sh

kill $tail_pid
wait $tail_pid || true

./test.sh
