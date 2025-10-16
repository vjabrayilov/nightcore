#!/bin/bash

set -euo pipefail

BASE_DIR=$(realpath $(dirname $0))

pushd $BASE_DIR >/dev/null

RUSTFLAGS="-C debuginfo=1" cargo build --release --manifest-path $BASE_DIR/foo/Cargo.toml
RUSTFLAGS="-C debuginfo=1" cargo build --release --manifest-path $BASE_DIR/bar/Cargo.toml

cp $BASE_DIR/target/release/libfoo.so $BASE_DIR/libfoo.so
cp $BASE_DIR/target/release/libbar.so $BASE_DIR/libbar.so

popd >/dev/null


