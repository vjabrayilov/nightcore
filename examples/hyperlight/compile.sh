#!/bin/bash

set -euo pipefail

BASE_DIR=$(realpath $(dirname $0))
GUEST_TARGET="x86_64-unknown-none"
GUEST_RUSTFLAGS="-C panic=abort -C code-model=small -C link-args=-eentrypoint"

echo "Building Hyperlight guest..."
cd $BASE_DIR/guest
RUSTFLAGS="$GUEST_RUSTFLAGS" cargo build --release --target $GUEST_TARGET

echo "Building Hyperlight worker wrapper..."
cd $BASE_DIR/worker
cargo build --release

# Copy artifacts to convenient locations
echo "Copying artifacts..."
cp $BASE_DIR/target/release/libhyperlight_worker.so $BASE_DIR/libecho.so
cp $BASE_DIR/target/hyperlight-guest/$GUEST_TARGET/release/hyperlight-example-guest $BASE_DIR/guest.bin

echo "Build complete!"
echo "  Worker library: $BASE_DIR/libecho.so"
echo "  Guest binary:   $BASE_DIR/guest.bin"
