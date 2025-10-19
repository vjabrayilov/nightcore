#!/bin/bash

# Example script to run Nightcore with Machnet kernel-bypass networking
# This demonstrates Gateway <-> Engine communication via Machnet instead of TCP
#
# Usage:
#   MODE=gateway ./run_stack_machnet.sh    # Run gateway only
#   MODE=worker ./run_stack_machnet.sh     # Run engine + launcher

BASE_DIR=$(pwd)/../../
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
LOG_DIR="$SCRIPT_DIR/logs"

# Deployment mode: "gateway" or "worker"
MODE="${MODE:-gateway}"

# Machnet configuration
# IMPORTANT: Update these IPs to match your Machnet-enabled NICs
GATEWAY_MACHNET_IP="${GATEWAY_MACHNET_IP:-10.10.1.1}"
ENGINE_MACHNET_IP="${ENGINE_MACHNET_IP:-10.10.1.2}"
MACHNET_PORT=10007

# Standard configuration
HTTP_PORT=8080
NODE_ID="${NODE_ID:-0}"
FUNC_CONFIG="$SCRIPT_DIR/func_config.json"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=========================================="
echo "Nightcore with Machnet - C Example"
echo "Mode: $MODE"
echo "=========================================="
echo ""

# Validate mode
if [ "$MODE" != "gateway" ] && [ "$MODE" != "worker" ]; then
    echo -e "${RED}Error: Invalid MODE=${MODE}${NC}"
    echo "Valid modes: gateway, worker"
    echo ""
    echo "Usage:"
    echo "  MODE=gateway $0   # Gateway node"
    echo "  MODE=worker $0    # Worker node (Engine + Launcher)"
    exit 1
fi

# Check if Machnet IPs are set
if [ "$GATEWAY_MACHNET_IP" = "10.0.1.10" ] || [ "$ENGINE_MACHNET_IP" = "10.0.1.11" ]; then
    echo -e "${YELLOW}Warning: Using default Machnet IPs${NC}"
    echo "Gateway Machnet IP: $GATEWAY_MACHNET_IP"
    echo "Engine Machnet IP: $ENGINE_MACHNET_IP"
    echo ""
    echo "To use your own IPs:"
    echo "  export GATEWAY_MACHNET_IP=<your_gateway_machnet_ip>"
    echo "  export ENGINE_MACHNET_IP=<your_engine_machnet_ip>"
    echo "  MODE=$MODE $0"
    echo ""
fi

# Check if binaries exist (based on mode)
if [ "$MODE" = "gateway" ]; then
    if [ ! -f "$BASE_DIR/bin/release/gateway" ]; then
        echo "$BASE_DIR/bin/release/gateway"
        echo -e "${RED}Error: Gateway binary not found${NC}"
        echo "Please build the project first:"
        echo "  make -j \$(nproc)"
        exit 1
    fi
fi

if [ "$MODE" = "worker" ]; then
    if [ ! -f "$BASE_DIR/bin/release/engine" ] || \
       [ ! -f "$BASE_DIR/bin/release/launcher" ]; then
        echo -e "${RED}Error: Engine/Launcher binaries not found${NC}"
        echo "Please build the project first:"
        echo "  make -j \$(nproc)"
        exit 1
    fi

    # Check if function library exists
    if [ ! -f "$SCRIPT_DIR/libfoo.so" ]; then
        echo -e "${YELLOW}Building function library...${NC}"
        cd "$SCRIPT_DIR"
        ./compile.sh
        cd "$BASE_DIR"
    fi
fi

# Check if Machnet is running
if ! pgrep -f "machnet" > /dev/null; then
    echo -e "${YELLOW}Warning: Machnet daemon not detected${NC}"
    echo "Please start Machnet first:"
    echo "  sudo examples/machnet_setup.sh"
    echo ""
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Cleanup function
cleanup() {
    echo ""
    echo "Shutting down..."
    pkill -P $$ || true
    sleep 1
    exit 0
}

trap cleanup INT TERM

# Create log directory
mkdir -p "$LOG_DIR"

echo -e "${GREEN}Starting Nightcore with Machnet (mode: $MODE)...${NC}"
echo "Log output directory: $LOG_DIR"
echo ""

STEP=1
TOTAL_STEPS=0
[ "$MODE" = "gateway" ] && TOTAL_STEPS=$((TOTAL_STEPS + 1))
[ "$MODE" = "worker" ] && TOTAL_STEPS=$((TOTAL_STEPS + 2))

# Start Gateway with Machnet (gateway mode only)
if [ "$MODE" = "gateway" ]; then
    echo "[$STEP/$TOTAL_STEPS] Starting Gateway (Machnet: $GATEWAY_MACHNET_IP:$MACHNET_PORT)..."
    $BASE_DIR/bin/release/gateway \
        --func_config_file="$FUNC_CONFIG" \
        --http_port=$HTTP_PORT \
        --grpc_port=50051 \
        --use_machnet=true \
        --machnet_ip="$GATEWAY_MACHNET_IP" \
        --engine_conn_port=$MACHNET_PORT \
        --num_io_workers=1 &

    GATEWAY_PID=$!
    sleep 2

    # Check if Gateway started
    if ! ps -p $GATEWAY_PID > /dev/null; then
        echo -e "${RED}Error: Gateway failed to start${NC}"
        echo "Check the logs above for details"
        exit 1
    fi
    echo "  ✓ Gateway started (PID: $GATEWAY_PID)"
    echo ""
    STEP=$((STEP + 1))
fi

# Start Engine with Machnet (worker mode only)
if [ "$MODE" = "worker" ]; then
    echo "[$STEP/$TOTAL_STEPS] Starting Engine (Machnet: $ENGINE_MACHNET_IP -> $GATEWAY_MACHNET_IP:$MACHNET_PORT)..."
    $BASE_DIR/bin/release/engine \
        --func_config_file="$FUNC_CONFIG" \
        --use_machnet=true \
        --machnet_ip="$ENGINE_MACHNET_IP" \
        --gateway_machnet_ip="$GATEWAY_MACHNET_IP" \
        --gateway_port=$MACHNET_PORT \
        --node_id=$NODE_ID \
        --num_io_workers=1 \
        --gateway_conn_per_worker=2 &

    ENGINE_PID=$!
    sleep 2

    # Check if Engine started
    if ! ps -p $ENGINE_PID > /dev/null; then
        echo -e "${RED}Error: Engine failed to start${NC}"
        echo "Check the logs above for details"
        [ -n "$GATEWAY_PID" ] && kill $GATEWAY_PID 2>/dev/null || true
        exit 1
    fi
    echo "  ✓ Engine started (PID: $ENGINE_PID)"
    echo ""
    STEP=$((STEP + 1))

    # Start Launcher
    echo "[$STEP/$TOTAL_STEPS] Starting Launcher (func_id=1)..."
    $BASE_DIR/bin/release/launcher \
        --func_id=1 \
        --fprocess_mode=cpp \
        --fprocess="$BASE_DIR/bin/release/func_worker_v1 $SCRIPT_DIR/libfoo.so" \
        --fprocess_output_dir="$LOG_DIR" &

    LAUNCHER_PID=$!
    sleep 2

    # Check if Launcher started
    if ! ps -p $LAUNCHER_PID > /dev/null; then
        echo -e "${RED}Error: Launcher failed to start${NC}"
        [ -n "$GATEWAY_PID" ] && kill $GATEWAY_PID 2>/dev/null || true
        [ -n "$ENGINE_PID" ] && kill $ENGINE_PID 2>/dev/null || true
        exit 1
    fi
    echo "  ✓ Launcher started (PID: $LAUNCHER_PID)"
    echo ""
fi

echo -e "${GREEN}=========================================="
echo "Components started successfully!"
echo "==========================================${NC}"
echo ""

# Display running components
if [ "$MODE" = "gateway" ]; then
    echo "Gateway:  http://localhost:$HTTP_PORT (Machnet: $GATEWAY_MACHNET_IP:$MACHNET_PORT)"
    echo ""
    echo "PID: $GATEWAY_PID"
    echo ""
    echo -e "${YELLOW}Note: Gateway is waiting for Engine connections${NC}"
    echo "Start Engine on worker node(s) with:"
    echo "  export GATEWAY_MACHNET_IP=$GATEWAY_MACHNET_IP"
    echo "  export ENGINE_MACHNET_IP=<worker_machnet_ip>"
    echo "  export NODE_ID=0  # Increment for each worker"
    echo "  MODE=worker $0"

elif [ "$MODE" = "worker" ]; then
    echo "Engine:   Machnet connection ($ENGINE_MACHNET_IP -> $GATEWAY_MACHNET_IP:$MACHNET_PORT)"
    echo "Launcher: func_id=1 (Foo), node_id=$NODE_ID"
    echo ""
    echo "PIDs:"
    echo "  Engine:   $ENGINE_PID"
    echo "  Launcher: $LAUNCHER_PID"
    echo ""
    echo -e "${YELLOW}Note: Worker node connected to Gateway at $GATEWAY_MACHNET_IP:$MACHNET_PORT${NC}"
fi

echo ""
echo "Press Ctrl+C to stop all components"

# Wait for user interrupt
wait
