#!/bin/bash

set -eu
set -o pipefail

cd $(realpath $(dirname ${BASH_SOURCE[0]}))

stamp=gem5.docker
touch $stamp
docker build .
DOCKER_IMAGE=$(docker build -q .)
apptainer build -F gem5.sif docker-daemon://$DOCKER_IMAGE
