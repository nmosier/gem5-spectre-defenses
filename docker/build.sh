#!/bin/bash

set -eu
set -o pipefail

cd $(realpath $(dirname ${BASH_SOURCE[0]}))

stamp=gem5.docker
touch $stamp
OLD_DOCKER_IMAGE=$(cat $stamp)
NEW_DOCKER_IMAGE=$(docker build -q . | tee $stamp)
if [[ "$OLD_DOCKER_IMAGE" != "$NEW_DOCKER_IMAGE" ]]; then
    echo "INFO: docker image changed; rebuilding apptainer ($OLD_DOCKER_IMAGE -> $NEW_DOCKER_IMAGE)" >&2
    apptainer build -F gem5.sif docker-daemon://$NEW_DOCKER_IMAGE
fi
