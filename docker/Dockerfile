FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=1

# Install dependencies.
RUN apt-get update && apt-get install -y \
    build-essential zlib1g-dev python3-dev \
    scons libgoogle-perftools-dev m4 git pkg-config ccache \
    python-is-python3 python3 python3-six

# Set up ccache.
ENV CCACHE_DIR=/ccache
ENV PATH="/usr/lib/ccache:$PATH"
RUN ccache --set-config=cache_dir=/ccache

# Set up m5cache.
ENV M5_BUILD_CACHE=/m5cache

RUN apt-get update && apt-get install -y gdb
