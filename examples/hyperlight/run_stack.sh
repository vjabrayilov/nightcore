#!/bin/bash

set -euo pipefail

BASE_DIR=$(realpath $(dirname $0))
NIGHTCORE_ROOT=$(realpath $(dirname $0)/../..)
BUILD_TYPE=release

# Set guest binary path for the worker
export HYPERLIGHT_GUEST_BIN_PATH=$BASE_DIR/guest.bin

rm -rf $BASE_DIR/outputs
mkdir -p $BASE_DIR/outputs

echo "Starting Nightcore stack with Hyperlight..."

$NIGHTCORE_ROOT/bin/$BUILD_TYPE/gateway \
    --func_config_file=$BASE_DIR/func_config.json \
    --v=1 2>$BASE_DIR/outputs/gateway.log &

sleep 1

$NIGHTCORE_ROOT/bin/$BUILD_TYPE/engine \
    --func_config_file=$BASE_DIR/func_config.json \
    --node_id=0 \
    --v=1 2>$BASE_DIR/outputs/engine.log &

sleep 1

$NIGHTCORE_ROOT/bin/$BUILD_TYPE/launcher \
    --func_id=1 --fprocess_mode=cpp \
    --fprocess_output_dir=$BASE_DIR/outputs \
    --fprocess="$NIGHTCORE_ROOT/bin/$BUILD_TYPE/func_worker_v1 $BASE_DIR/libecho.so" \
    --v=1 2>$BASE_DIR/outputs/launcher_echo.log &

echo "Nightcore stack started. Logs in $BASE_DIR/outputs/"
echo ""
echo "Test with:"
echo "  curl -X POST -d \"hello\" http://127.0.0.1:8080/function/Echo"
echo ""
echo "Press Ctrl+C to stop all processes"

wait
