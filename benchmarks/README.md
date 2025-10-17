# Nightcore Stress Test

This benchmark stress tests Nightcore with different function implementations to measure performance characteristics.

## Architecture

```
stress_client (listens on :10007)
      ↓ (Engine connects)
    Engine
      ↓ (IPC)
   Launcher
      ↓ (spawns)
   Workers (execute noop)
```

The `stress_client` **emulates a Gateway** - it listens for Engine connections and generates high-volume load with precise statistics tracking.

## Modes

The benchmark supports two modes to measure different aspects of performance:

### Noop Mode (Default)
- **Function**: Minimal noop function that returns immediately
- **Purpose**: Measures pure Nightcore overhead (IPC, scheduling, message passing)
- **Library**: `libnoop.so` (built from `noop.c`)

### Hyperlight Mode
- **Function**: Echo function running in Hyperlight KVM-based sandbox
- **Purpose**: Measures Nightcore + hypervisor isolation overhead
- **Library**: `examples/hyperlight/libecho.so` + `guest.bin`
- **Requires**: Hyperlight example must be built first, KVM support

## Components

1. **noop.c** - Minimal function that returns immediately (noop mode)
2. **stress_client** - High-performance load generator with per-thread statistics
3. **func_config.json** - Configuration for the function (auto-generated based on mode)
4. **Hyperlight integration** - Optional, uses sandboxed Echo function (hyperlight mode)

## Building

### Noop Mode

```bash
# From the Nightcore root directory
make -j $(nproc)

# Compile the noop function
cd benchmarks/noop
./compile.sh
```

### Hyperlight Mode

```bash
# Build Nightcore
make -j $(nproc)

# Build Hyperlight example
cd examples/hyperlight
make

# Noop function is not needed for hyperlight mode
```

## Running

### Noop Mode (Default)

```bash
# Default: noop mode, 16 workers, 30s duration
./run_bench.sh

# Named-arg syntax (recommended)
./run_bench.sh \
  --mode noop \
  --workers 16 \
  --duration 30 \
  --target_rps 0 \
  --input_size 64 \
  --inflight_limit 1 \
  --warmup_sec 0 \
  --num_io_workers 1 \
  --gateway_conn_per_worker 1

# Legacy positional syntax (still supported, always uses noop mode)
./run_bench.sh [workers] [duration] [target_rps] [input_size] [inflight_limit] [warmup_sec]
```

### Hyperlight Mode

```bash
# Basic hyperlight test
./run_bench.sh --mode hyperlight --workers 16 --duration 30

# Hyperlight with high concurrency
./run_bench.sh --mode hyperlight --workers 32 --duration 60 --inflight_limit 2000

# Hyperlight with rate limiting
./run_bench.sh --mode hyperlight --workers 16 --duration 60 --target_rps 10000

# Hyperlight with multiple connections
./run_bench.sh --mode hyperlight --workers 16 --duration 30 --num_io_workers 2 --gateway_conn_per_worker 2
```

### Comparison Examples

```bash
# Compare noop vs hyperlight baseline
./run_bench.sh --mode noop --workers 16 --duration 30 --inflight_limit 1000
./run_bench.sh --mode hyperlight --workers 16 --duration 30 --inflight_limit 1000

# Compare with different payloads
./run_bench.sh --mode noop --workers 16 --duration 30 --input_size 1024
./run_bench.sh --mode hyperlight --workers 16 --duration 30 --input_size 1024
```

## Parameters

- **--mode**: Test mode - `noop` or `hyperlight` (default: noop)
- **--workers**: number of function workers (default: 16)
- **--duration**: test duration in seconds (default: 30)
- **--target_rps**: GLOBAL target RPS across all connections; 0 = unlimited (default: 0)
- **--input_size**: payload size in bytes (default: 64)
- **--inflight_limit**: max inflight requests per connection (default: 1)
- **--warmup_sec**: warmup period before measuring (default: 0)
- **--num_io_workers**: Engine IO workers (default: 1)
- **--gateway_conn_per_worker**: Gateway connections per IO worker (default: 1)

Number of Engine→client connections = `num_io_workers × gateway_conn_per_worker`.

### Mode-Specific Behavior

| Aspect | Noop Mode | Hyperlight Mode |
|--------|-----------|-----------------|
| Function | Noop (instant return) | Echo (KVM sandbox) |
| Library | `libnoop.so` | `libecho.so` + `guest.bin` |
| Measures | Pure Nightcore | Nightcore + KVM |
| Prerequisites | `cd noop && ./compile.sh` | `cd ../examples/hyperlight && make` |

## Key Design Features

1. **Non-blocking polling**: All sender threads use 10μs sleep polling (no blocking waits)
2. **Per-thread statistics**: Zero-lock accounting during test, merged at end
3. **Warmup phase**: Latencies only recorded after warmup completes
4. **Open-loop mode**: Each thread maintains up to `INFLIGHT` concurrent requests
5. **Rate limiting**: Optional precise RPS pacing per thread

## How RPS works

- `--target_rps` is treated as a **global** rate by `stress_client`.
- If there are \(N\) active connections from Engine, per-connection rate is:
  - `floor(target_rps / N)` for most connections, and the first `target_rps % N` connections get `+1`.
- If `--target_rps=0`, each connection sends at unlimited rate (bounded only by `--inflight_limit`).

## Output

**During test (every 1 second):**
```
[T+5s] Sent: 15234 | Completed: 15128 | Failed: 0 | Latencies collected: 14892
```

**Final summary:**
```
========================================
=== Stress Test Results ===
========================================
Duration:          30.00 seconds
Total Sent:        456789
Total Completed:   456234 (99.88%)
Total Failed:      45 (0.01%)

Throughput:        15207.8 rps

Latency (us):
  Min:    178
  p50:    2134
  p90:    4892
  p99:    8401
  p99.9:  15234
  Max:    27891
========================================
```

Worker output saved to: `outputs/Noop_worker_*.stdout`

## Advanced Usage

Run `stress_client` directly for custom configurations:

```bash
bin/release/stress_client \
    --listen_port=10007 \
    --func_id=1 \
    --duration_sec=60 \
    --target_rps=5000 \
    --input_size=128 \
    --inflight_limit=2000 \
    --warmup_sec=10
```

Notes:
- `--target_rps` is global and will be divided among the connections that Engine establishes.
- The number of connections is controlled by Engine via `--num_io_workers` and `--gateway_conn_per_worker`.

See all flags with: `bin/release/stress_client --help`

## Tuning Tips

**For maximum throughput:**
- Start with 1 sender thread
- Set `--target_rps=0` (unlimited)
- Increase `--inflight_limit` to 5000-10000
- Match workers to CPU cores

**For realistic workload:**
- Use multiple connections by increasing Engine `--num_io_workers` and `--gateway_conn_per_worker`.
- Set `--target_rps` to desired global load.
- Use `--warmup_sec=10` for stability.

**For large payloads:**
- Increase `--input_size` (e.g., 4096)
- May need to reduce inflight limit

## Example Experiments

### Noop Mode Experiments

```bash
# 1. Maximum throughput (noop)
./run_bench.sh --mode noop --workers 16 --duration 30 --target_rps 0 --inflight_limit 5000 --warmup_sec 5

# 2. Multi-connection concurrency test (4 connections, noop)
./run_bench.sh --mode noop --workers 16 --duration 30 --target_rps 0 --gateway_conn_per_worker 2 --num_io_workers 2 --inflight_limit 1000 --warmup_sec 5

# 3. Rate-limited sustained load (10K global RPS, noop)
./run_bench.sh --mode noop --workers 16 --duration 60 --target_rps 10000 --gateway_conn_per_worker 2 --num_io_workers 2 --input_size 64 --inflight_limit 1000 --warmup_sec 10

# 4. Large payload test (noop)
./run_bench.sh --mode noop --workers 16 --duration 30 --target_rps 0 --input_size 4096 --inflight_limit 1000 --warmup_sec 5
```

### Hyperlight Mode Experiments

```bash
# 1. Maximum throughput (hyperlight)
./run_bench.sh --mode hyperlight --workers 16 --duration 30 --target_rps 0 --inflight_limit 5000 --warmup_sec 5

# 2. Hyperlight isolation overhead measurement
./run_bench.sh --mode hyperlight --workers 16 --duration 60 --target_rps 5000 --inflight_limit 1000 --warmup_sec 10

# 3. High worker count test (KVM scalability)
./run_bench.sh --mode hyperlight --workers 32 --duration 30 --target_rps 0 --inflight_limit 2000 --warmup_sec 5

# 4. Sustained load with Hyperlight
./run_bench.sh --mode hyperlight --workers 24 --duration 120 --target_rps 8000 --num_io_workers 2 --gateway_conn_per_worker 2 --warmup_sec 15
```

### Comparison Experiments

```bash
# Compare baseline performance
./run_bench.sh --mode noop --workers 16 --duration 30 --inflight_limit 1000 > noop_results.txt
./run_bench.sh --mode hyperlight --workers 16 --duration 30 --inflight_limit 1000 > hyperlight_results.txt

# Compare with same rate limiting
./run_bench.sh --mode noop --workers 16 --duration 60 --target_rps 5000 --warmup_sec 10 > noop_5k.txt
./run_bench.sh --mode hyperlight --workers 16 --duration 60 --target_rps 5000 --warmup_sec 10 > hyperlight_5k.txt
```
