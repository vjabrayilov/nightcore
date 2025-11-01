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

# Remote stress_client configuration
REMOTE_STRESS_CLIENT=false    # Run stress_client on remote machine
REMOTE_HOST=""                # Remote host for stress_client (e.g., user@hostname)
REMOTE_ADDR=""                # Remote IP/hostname for Engine connection (default: extract from REMOTE_HOST)
REMOTE_NIGHTCORE_ROOT=""      # Nightcore root on remote (default: same as local)

# Machnet configuration
USE_MACHNET=false             # Use Machnet transport (requires --remote and --remote_addr)
LOCAL_MACHNET_IP=""           # Local Machnet IP for Engine (default: use system default)

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

  Distributed Setup:
  --remote HOST                Run stress_client on remote host (e.g., user@hostname)
  --remote_addr IP             IP/hostname for Engine to connect to remote (default: extract from --remote)
  --remote_root PATH           Nightcore root on remote host (default: same as local)

  Machnet (requires --remote and --remote_addr):
  --machnet                    Use Machnet transport instead of TCP
  --local_machnet_ip IP        Local Machnet IP for Engine (optional)

  -h, --help                   Show this help and exit

Modes:
  noop        - Minimal noop function (default)
  hyperlight  - Hyperlight sandboxed Echo function (requires hyperlight example built)

Examples:
  # Local benchmark
  $0 --mode noop --workers 16 --duration 30

  # Remote stress_client (requires ssh access and same directory structure)
  $0 --mode noop --workers 16 --duration 30 --remote user@gateway-host

  # Remote with explicit internal IP (e.g., CloudLab)
  $0 --mode noop --workers 16 --duration 30 \\
     --remote user@sm110p.cloudlab.us --remote_addr 10.10.1.2

  # Remote with Machnet transport
  $0 --mode noop --workers 16 --duration 30 \\
     --remote user@sm110p.cloudlab.us --remote_addr 10.10.1.2 \\
     --machnet --local_machnet_ip 10.10.1.1

Positional legacy args (still supported):
  [workers] [duration] [target_rps] [input_size] [inflight_limit] [warmup_sec]
USAGE
}

# Parse named args first, but keep legacy positionals as fallback
POSITIONAL=()
NAMED_ARGS_USED=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --workers)
      NUM_WORKERS="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --duration)
      DURATION="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --target_rps)
      TARGET_RPS="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --input_size)
      INPUT_SIZE="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --inflight_limit)
      INFLIGHT_LIMIT="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --warmup_sec)
      WARMUP_SEC="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --num_io_workers)
      NUM_IO_WORKERS="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --gateway_conn_per_worker)
      GATEWAY_CONN_PER_WORKER="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --remote)
      REMOTE_STRESS_CLIENT=true; REMOTE_HOST="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --remote_addr)
      REMOTE_ADDR="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --remote_root)
      REMOTE_NIGHTCORE_ROOT="$2"; NAMED_ARGS_USED=true; shift 2 ;;
    --machnet)
      USE_MACHNET=true; NAMED_ARGS_USED=true; shift ;;
    --local_machnet_ip)
      LOCAL_MACHNET_IP="$2"; NAMED_ARGS_USED=true; shift 2 ;;
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

# Legacy positional support (only if no named args were used)
if [[ ${#POSITIONAL[@]} -gt 0 ]]; then
  if [ "$NAMED_ARGS_USED" = "true" ]; then
    echo "WARNING: Ignoring unexpected positional arguments: ${POSITIONAL[@]}"
    echo "When using named options (--workers, --duration, etc), do not mix with positional args."
    echo ""
  else
    # Apply legacy positional arguments
    [[ -n "${POSITIONAL[0]}" ]] && NUM_WORKERS="${POSITIONAL[0]}"
    [[ -n "${POSITIONAL[1]}" ]] && DURATION="${POSITIONAL[1]}"
    [[ -n "${POSITIONAL[2]}" ]] && TARGET_RPS="${POSITIONAL[2]}"
    [[ -n "${POSITIONAL[3]}" ]] && INPUT_SIZE="${POSITIONAL[3]}"
    [[ -n "${POSITIONAL[4]}" ]] && INFLIGHT_LIMIT="${POSITIONAL[4]}"
    [[ -n "${POSITIONAL[5]}" ]] && WARMUP_SEC="${POSITIONAL[5]}"
  fi
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

# Validate Machnet configuration
if [ "$USE_MACHNET" = "true" ]; then
  if [ "$REMOTE_STRESS_CLIENT" != "true" ]; then
    echo "ERROR: --machnet requires --remote to be specified"
    echo ""
    usage
    exit 1
  fi

  if [ -z "$REMOTE_ADDR" ]; then
    echo "ERROR: --machnet requires --remote_addr to be specified"
    echo "Machnet requires explicit IP addresses for both local and remote endpoints"
    echo ""
    usage
    exit 1
  fi
fi

# Validate and set remote configuration
if [ "$REMOTE_STRESS_CLIENT" = "true" ]; then
  # Validate REMOTE_HOST format
  if [[ ! "$REMOTE_HOST" =~ @ ]]; then
    echo "ERROR: Invalid --remote HOST format: '$REMOTE_HOST'"
    echo "Expected format: user@hostname (e.g., user@host.example.com)"
    echo ""
    echo "Did you mean: --remote $REMOTE_HOST (without 'ssh' prefix)?"
    exit 1
  fi

  # Set remote nightcore root if not specified
  if [ -z "$REMOTE_NIGHTCORE_ROOT" ]; then
    REMOTE_NIGHTCORE_ROOT="$NIGHTCORE_ROOT"
  fi
fi

# Determine stress_client address for Engine connection
if [ "$REMOTE_STRESS_CLIENT" = "true" ]; then
  # Use explicitly provided address, or extract hostname from user@host format
  if [ -n "$REMOTE_ADDR" ]; then
    STRESS_CLIENT_ADDR="$REMOTE_ADDR"
  else
    STRESS_CLIENT_ADDR="${REMOTE_HOST##*@}"
  fi
else
  STRESS_CLIENT_ADDR="127.0.0.1"
fi

# Print configuration summary
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
if [ "$REMOTE_STRESS_CLIENT" = "true" ]; then
  echo "Stress Client: Remote ($REMOTE_HOST)"
  if [ -n "$REMOTE_ADDR" ]; then
    echo "Engine Connection Address: $STRESS_CLIENT_ADDR (explicit)"
  else
    echo "Engine Connection Address: $STRESS_CLIENT_ADDR (auto-detected)"
  fi
else
  echo "Stress Client: Local"
fi

if [ "$USE_MACHNET" = "true" ]; then
  echo "Transport: Machnet"
  echo "  Remote Machnet IP: $STRESS_CLIENT_ADDR"
  if [ -n "$LOCAL_MACHNET_IP" ]; then
    echo "  Local Machnet IP: $LOCAL_MACHNET_IP"
  else
    echo "  Local Machnet IP: (auto-detected)"
  fi
else
  echo "Transport: TCP"
fi
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

STRESS_CLIENT_PID=""

if [ "$REMOTE_STRESS_CLIENT" = "true" ]; then
  # Clean up any existing stress_client on remote
  echo "Cleaning up any existing stress_client on remote..."
  ssh "$REMOTE_HOST" "pkill -9 stress_client 2>/dev/null || true"
  sleep 1

  # Start stress_client on remote machine
  echo "Starting stress_client on remote host $REMOTE_HOST..."

  REMOTE_OUTPUT="$REMOTE_NIGHTCORE_ROOT/benchmarks/outputs/stress_client_remote.txt"

  # Build stress_client command with optional Machnet flags
  STRESS_CLIENT_CMD="bin/$BUILD_TYPE/stress_client \
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
      --v=0"

  if [ "$USE_MACHNET" = "true" ]; then
    STRESS_CLIENT_CMD="$STRESS_CLIENT_CMD --use_machnet=true --machnet_ip=$STRESS_CLIENT_ADDR"
  fi

  # Run stress_client remotely in blocking mode via SSH
  # The SSH process will block until stress_client completes
  ssh "$REMOTE_HOST" "mkdir -p $REMOTE_NIGHTCORE_ROOT/benchmarks/outputs && \
    cd $REMOTE_NIGHTCORE_ROOT && \
    $STRESS_CLIENT_CMD 2>&1 | tee $REMOTE_OUTPUT" &

  STRESS_CLIENT_PID=$!

  echo "  Remote SSH process PID: $STRESS_CLIENT_PID"
  echo "  Output will be saved to: $REMOTE_OUTPUT"
  echo "Waiting for remote stress_client to start..."
  sleep 3
else
  # Start stress_client locally
  echo "Starting stress_client locally (listens on port 10007)..."
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
      --v=0 2>&1 | tee $BASE_DIR/outputs/stress_client.txt &
  STRESS_CLIENT_PID=$!

  echo "Waiting for stress_client to start listening..."
  sleep 2
fi

# Start Engine (connects to stress_client)
echo "Starting Engine (connecting to $STRESS_CLIENT_ADDR:10007)..."

# Build engine command with optional Machnet flags
ENGINE_CMD="$NIGHTCORE_ROOT/bin/$BUILD_TYPE/engine \
    --func_config_file=$BASE_DIR/func_config.json \
    --node_id=0 \
    --gateway_port=10007 \
    --num_io_workers=$NUM_IO_WORKERS \
    --gateway_conn_per_worker=$GATEWAY_CONN_PER_WORKER \
    --v=0"

if [ "$USE_MACHNET" = "true" ]; then
  ENGINE_CMD="$ENGINE_CMD --use_machnet=true --gateway_machnet_ip=$STRESS_CLIENT_ADDR"
  if [ -n "$LOCAL_MACHNET_IP" ]; then
    ENGINE_CMD="$ENGINE_CMD --machnet_ip=$LOCAL_MACHNET_IP"
  fi
else
  ENGINE_CMD="$ENGINE_CMD --gateway_addr=$STRESS_CLIENT_ADDR"
fi

$ENGINE_CMD > $BASE_DIR/outputs/engine.log 2>&1 &

ENGINE_PID=$!
echo "  Engine PID: $ENGINE_PID"

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
    --v=0 > $BASE_DIR/outputs/launcher.log 2>&1 &
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

# Copy remote output if running in remote mode
if [ "$REMOTE_STRESS_CLIENT" = "true" ]; then
  echo ""
  echo "Copying results from remote host..."
  scp "$REMOTE_HOST:$REMOTE_OUTPUT" "$BASE_DIR/outputs/stress_client.txt" 2>/dev/null

  if [ $? -eq 0 ]; then
    echo "Remote stress_client output:"
    cat "$BASE_DIR/outputs/stress_client.txt"
  else
    echo "WARNING: Failed to copy remote output from $REMOTE_HOST:$REMOTE_OUTPUT"
  fi

  echo ""
  echo "Cleaning up any remaining remote stress_client processes..."
  ssh "$REMOTE_HOST" "pkill -9 stress_client 2>/dev/null || true"
fi

echo ""
echo "Logs saved to:"
echo "  Engine:   $BASE_DIR/outputs/engine.log"
echo "  Launcher: $BASE_DIR/outputs/launcher.log"
echo "  Workers:  $BASE_DIR/outputs/${FUNC_NAME}_worker_*.stdout"

exit $BENCH_EXIT_CODE
