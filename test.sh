#!/bin/sh

set -eu

./build.sh
./tests/test_build_pack.sh
