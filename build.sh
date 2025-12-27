#!/bin/bash

set -eu

root=$(dirname ${BASH_SOURCE[0]})

$root/docker/exec.sh scons --ignore-style -j`nproc` M5_BUILD_CACHE=/m5cache build/X86_MESI_Two_Level/gem5.opt
# build/X86/compile_commands.json
