#!/bin/bash

BASE_DIR=$(realpath $(dirname $0))
NIGHTCORE_ROOT=$(realpath $BASE_DIR/../..)
BUILD_TYPE=release

# Check if number of workers is specified
NUM_WORKERS=${1:-16}
DURATION=${2:-30}
NUM_CLIENTS=${3:-20}
TARGET_RPS=${4:-0}

echo "=== Nightcore Noop Stress Test (Direct to Engine) ==="
echo "Workers: $NUM_WORKERS"
echo "Duration: $DURATION seconds"
echo "Client threads: $NUM_CLIENTS"
echo "Target RPS per thread: $([ $TARGET_RPS -eq 0 ] && echo 'unlimited' || echo $TARGET_RPS)"
echo ""
echo "NOTE: No Gateway - stress_client connects directly to Engine"
echo ""

# Clean up previous run
rm -rf $BASE_DIR/outputs
mkdir -p $BASE_DIR/outputs

# Update func_config.json with requested number of workers
cat > $BASE_DIR/func_config.json <<EOF
[
  {
    "funcName": "Noop",
    "funcId": 1,
    "minWorkers": $NUM_WORKERS,
    "maxWorkers": $NUM_WORKERS
  }
]
EOF

# Start stress_client FIRST (it needs to be listening before Engine connects)
echo "Starting stress client (acting as gateway)..."
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/stress_client \
    --listen_addr=0.0.0.0 \
    --listen_port=10007 \
    --num_sender_threads=$NUM_CLIENTS \
    --duration_sec=$DURATION \
    --target_rps=$TARGET_RPS \
    --func_id=1 \
    --v=0 2>&1 &
STRESS_CLIENT_PID=$!

echo "Waiting for stress_client to start listening..."
sleep 2

# Start Engine (it will connect to stress_client)
echo "Starting Engine..."
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/engine \
    --func_config_file=$BASE_DIR/func_config.json \
    --node_id=0 \
    --gateway_addr=127.0.0.1 \
    --gateway_port=10007 \
    --v=0 2>/dev/null &
ENGINE_PID=$!

sleep 2

# Start Launcher
echo "Starting Launcher with $NUM_WORKERS workers..."
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/launcher \
    --func_id=1 --fprocess_mode=cpp \
    --fprocess_output_dir=$BASE_DIR/outputs \
    --fprocess="$NIGHTCORE_ROOT/bin/$BUILD_TYPE/func_worker_v1 $BASE_DIR/libnoop.so" \
    --v=0 2>/dev/null &
LAUNCHER_PID=$!

echo "All components started. Waiting for workers to initialize and load test to run..."
sleep 3

# Wait for stress_client to finish (it runs in background)
wait $STRESS_CLIENT_PID

BENCH_EXIT_CODE=$?

echo ""
echo "Stress test completed. Shutting down..."

# Gracefully shutdown components
kill -SIGINT $LAUNCHER_PID 2>/dev/null
kill -SIGINT $ENGINE_PID 2>/dev/null

# Wait a bit for graceful shutdown
sleep 2

# Force kill if still running
kill -9 $LAUNCHER_PID 2>/dev/null
kill -9 $ENGINE_PID 2>/dev/null

echo ""
echo "NOTE: Engine and Launcher logs suppressed for performance (2>/dev/null)"
echo "Worker stdout/stderr (if any): $BASE_DIR/outputs/Noop_worker_*.stdout"

exit $BENCH_EXIT_CODE
