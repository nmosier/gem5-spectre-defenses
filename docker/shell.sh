#!/bin/bash

set -eu
set -o pipefail

root=$(realpath $(dirname ${BASH_SOURCE[0]}))

host_m5cache=$root/../m5cache
mkdir -p $host_m5cache

apptainer shell \
	  --bind=$HOME/.ccache:/ccache \
	  --bind=$host_m5cache:/m5cache \
	  --bind=$(dirname $root) \
	  $root/gem5.sif "$@"
