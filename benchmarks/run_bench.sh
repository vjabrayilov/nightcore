#!/bin/bash

BASE_DIR=$(realpath $(dirname $0))
NIGHTCORE_ROOT=$(realpath $BASE_DIR/..)
BUILD_TYPE=release

# Defaults
MODE="noop"
NUM_WORKERS=16
DURATION=30
TARGET_RPS=0            # Global target RPS across all connections (0 = unlimited)
INPUT_SIZE=64
INFLIGHT_LIMIT=1
WARMUP_SEC=0
NUM_IO_WORKERS=1
GATEWAY_CONN_PER_WORKER=1

# Mode-specific variables (set by setup_<mode>_mode functions)
FUNC_NAME=""
LIB_PATH=""
EXTRA_ENV=""

usage() {
  cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --mode <noop|hyperlight>     Test mode (default: $MODE)
  --workers N                  Number of function workers (default: $NUM_WORKERS)
  --duration SEC               Test duration seconds (default: $DURATION)
  --target_rps N               Global target RPS across all connections (0=unlimited, default: $TARGET_RPS)
  --input_size BYTES           Input payload size (default: $INPUT_SIZE)
  --inflight_limit N           Max inflight per connection (default: $INFLIGHT_LIMIT)
  --warmup_sec SEC             Warmup seconds before measurement (default: $WARMUP_SEC)
  --num_io_workers N           Engine IO workers (default: $NUM_IO_WORKERS)
  --gateway_conn_per_worker N  Gateway connections per IO worker (default: $GATEWAY_CONN_PER_WORKER)
  -h, --help                   Show this help and exit

Modes:
  noop        - Minimal noop function (default)
  hyperlight  - Hyperlight sandboxed Echo function (requires hyperlight example built)

Positional legacy args (still supported):
  [workers] [duration] [target_rps] [input_size] [inflight_limit] [warmup_sec]
USAGE
}

# Parse named args first, but keep legacy positionals as fallback
POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"; shift 2 ;;
    --workers)
      NUM_WORKERS="$2"; shift 2 ;;
    --duration)
      DURATION="$2"; shift 2 ;;
    --target_rps)
      TARGET_RPS="$2"; shift 2 ;;
    --input_size)
      INPUT_SIZE="$2"; shift 2 ;;
    --inflight_limit)
      INFLIGHT_LIMIT="$2"; shift 2 ;;
    --warmup_sec)
      WARMUP_SEC="$2"; shift 2 ;;
    --num_io_workers)
      NUM_IO_WORKERS="$2"; shift 2 ;;
    --gateway_conn_per_worker)
      GATEWAY_CONN_PER_WORKER="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    --)
      shift; break ;;
    -*)
      echo "Unknown option: $1"; echo ""; usage; exit 1 ;;
    *)
      POSITIONAL+=("$1"); shift ;;
  esac
done

# Legacy positional support
if [[ ${#POSITIONAL[@]} -gt 0 ]]; then
  [[ -n "${POSITIONAL[0]}" ]] && NUM_WORKERS="${POSITIONAL[0]}"
  [[ -n "${POSITIONAL[1]}" ]] && DURATION="${POSITIONAL[1]}"
  [[ -n "${POSITIONAL[2]}" ]] && TARGET_RPS="${POSITIONAL[2]}"
  [[ -n "${POSITIONAL[3]}" ]] && INPUT_SIZE="${POSITIONAL[3]}"
  [[ -n "${POSITIONAL[4]}" ]] && INFLIGHT_LIMIT="${POSITIONAL[4]}"
  [[ -n "${POSITIONAL[5]}" ]] && WARMUP_SEC="${POSITIONAL[5]}"
fi

# Mode-specific setup functions
setup_noop_mode() {
  FUNC_NAME="Noop"
  LIB_PATH="$BASE_DIR/noop/libnoop.so"
  EXTRA_ENV=""

  if [[ ! -f "$LIB_PATH" ]]; then
    echo "ERROR: $LIB_PATH not found."
    echo "Build the noop function first:"
    echo "  cd $BASE_DIR/noop"
    echo "  ./compile.sh"
    exit 1
  fi
}

setup_hyperlight_mode() {
  FUNC_NAME="Echo"
  LIB_PATH="$NIGHTCORE_ROOT/examples/hyperlight/libecho.so"
  EXTRA_ENV="HYPERLIGHT_GUEST_BIN_PATH=$NIGHTCORE_ROOT/examples/hyperlight/guest.bin"

  if [[ ! -f "$LIB_PATH" ]]; then
    echo "ERROR: $LIB_PATH not found."
    echo "Build the hyperlight example first:"
    echo "  cd $NIGHTCORE_ROOT/examples/hyperlight"
    echo "  make"
    exit 1
  fi

  if [[ ! -f "$NIGHTCORE_ROOT/examples/hyperlight/guest.bin" ]]; then
    echo "ERROR: guest.bin not found."
    echo "Build the hyperlight example first:"
    echo "  cd $NIGHTCORE_ROOT/examples/hyperlight"
    echo "  make"
    exit 1
  fi
}

# Setup mode-specific configuration
case "$MODE" in
  noop)
    setup_noop_mode ;;
  hyperlight)
    setup_hyperlight_mode ;;
  *)
    echo "ERROR: Invalid mode '$MODE'. Use 'noop' or 'hyperlight'."
    echo ""
    usage
    exit 1
    ;;
esac

echo "=== Nightcore Stress Test ($MODE mode) ==="
echo "Mode: $MODE"
echo "Function: $FUNC_NAME"
echo "Workers: $NUM_WORKERS"
echo "Duration: $DURATION seconds"
echo "Target RPS (global): $([ "$TARGET_RPS" -eq 0 ] && echo 'unlimited' || echo $TARGET_RPS)"
echo "Input size: $INPUT_SIZE bytes"
echo "Inflight limit per connection: $INFLIGHT_LIMIT"
echo "Engine IO workers: $NUM_IO_WORKERS"
echo "Gateway conns per worker: $GATEWAY_CONN_PER_WORKER"
echo "Warmup: $WARMUP_SEC seconds"
echo ""

# Clean up previous run
rm -rf $BASE_DIR/outputs
mkdir -p $BASE_DIR/outputs

# Update func_config.json with requested number of workers
cat > $BASE_DIR/func_config.json <<EOF
[
  {
    "funcName": "$FUNC_NAME",
    "funcId": 1,
    "minWorkers": $NUM_WORKERS,
    "maxWorkers": $NUM_WORKERS
  }
]
EOF

# Start stress_client FIRST (it needs to be listening before Engine connects)
echo "Starting stress_client (listens on port 10007)..."
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/stress_client \
    --listen_addr=0.0.0.0 \
    --listen_port=10007 \
    --func_id=1 \
    --method_id=0 \
    --duration_sec=$DURATION \
    --target_rps=$TARGET_RPS \
    --input_size=$INPUT_SIZE \
    --inflight_limit=$INFLIGHT_LIMIT \
    --warmup_sec=$WARMUP_SEC \
    --report_interval_sec=1 \
    --v=0 2>&1 &
STRESS_CLIENT_PID=$!

echo "Waiting for stress_client to start listening..."
sleep 2

# Start Engine (it will connect to stress_client as if it were the gateway)
echo "Starting Engine..."
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/engine \
    --func_config_file=$BASE_DIR/func_config.json \
    --node_id=0 \
    --gateway_addr=127.0.0.1 \
    --gateway_port=10007 \
    --num_io_workers=$NUM_IO_WORKERS \
    --gateway_conn_per_worker=$GATEWAY_CONN_PER_WORKER \
    --v=0 > /dev/null 2>&1 &

ENGINE_PID=$!

sleep 2

# Start Launcher (with mode-specific environment if needed)
echo "Starting Launcher with $NUM_WORKERS workers..."
if [[ -n "$EXTRA_ENV" ]]; then
  export $EXTRA_ENV
fi
$NIGHTCORE_ROOT/bin/$BUILD_TYPE/launcher \
    --func_id=1 \
    --fprocess_mode=cpp \
    --fprocess_output_dir=$BASE_DIR/outputs \
    --fprocess="$NIGHTCORE_ROOT/bin/$BUILD_TYPE/func_worker_v1 $LIB_PATH" \
    --v=0 > /dev/null 2>&1 &
LAUNCHER_PID=$!

echo "All components started. Stress test will begin after warmup..."
echo ""

# Wait for stress_client to finish (it will run the test and print results)
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
echo "NOTE: Engine and Launcher logs suppressed (2>/dev/null)"
echo "Worker output: $BASE_DIR/outputs/${FUNC_NAME}_worker_*.stdout"

exit $BENCH_EXIT_CODE
