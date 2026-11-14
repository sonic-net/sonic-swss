#!/bin/bash
#
# Canonical sonic-swss package build, used by CI and local development.
set -ex

cd "$(dirname "$0")/.."

case "${GCOV:-${ENABLE_GCOV:-false}}" in
  [Tt]rue|1|yes|y)
    export ENABLE_GCOV=y
    echo "BUILD_DIR=$(pwd)" > build.info
    ;;
esac

case "${ASAN:-${ENABLE_ASAN:-false}}" in
  [Tt]rue|1|yes|y) export ENABLE_ASAN=y ;;
esac

rm -f ../swss_*.deb ../swss-dbg_*.deb
./autogen.sh
RUSTFLAGS=-Dwarnings dpkg-buildpackage -us -uc -b -j"$(nproc)"
cp ../*.deb .

RUSTFLAGS=-Dwarnings cargo test
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 300s cargo test --features local-storage-benchmark --bench local_storage_perf -- --self-check
