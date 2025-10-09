# Nightcore Noop Stress Test

This benchmark stress tests Nightcore by invoking a noop function that just prints "Hello World" to stdout.

## Architecture

```
stress_client (acts as Gateway) ← Engine ← Launcher ← Workers
      ↑ listens on port 10007      connects
```

The stress_client **acts as a Gateway** - it listens for Engine connections and stress tests the system with multiple sender threads.

## Components

1. **noop.c** - A minimal function that prints "Hello World" and returns
2. **stress_client** - Multi-threaded stress tester that acts as Gateway (accepts Engine connections)
3. **func_config.json** - Configuration for the noop function

## Building

```bash
# From the Nightcore root directory
make -j $(nproc)

# Compile the noop function
cd benchmarks/noop_stress
./compile.sh
```

## Running

```bash
# Basic usage (16 workers, 30 second duration, 20 client threads, unlimited RPS)
./run_bench.sh

# Custom configuration
./run_bench.sh <num_workers> <duration_sec> <num_client_threads> <target_rps_per_thread>

# Examples:
./run_bench.sh 8 60 20 0        # 8 workers, 60 seconds, 20 clients, unlimited RPS
./run_bench.sh 16 30 40 1000    # 16 workers, 30 seconds, 40 clients, 1000 RPS per client
```

## Parameters

- **num_workers**: Number of worker processes to spawn (default: 16)
- **duration_sec**: Test duration in seconds (default: 30)
- **num_client_threads**: Number of concurrent client threads sending requests (default: 20)
- **target_rps_per_thread**: Target requests per second per thread, 0 for unlimited (default: 0)

## Output

The benchmark will print:
- Total requests sent
- Total responses received
- Total errors
- Throughput (requests/second)
- Average latency (microseconds)
- Min/Max latency
- Success rate

Log files are saved in `outputs/`:
- `gateway.log` - Gateway logs
- `engine.log` - Engine logs (contains statistics)
- `launcher.log` - Launcher logs
- `Noop_worker_*.stdout` - Worker stdout (Hello World messages)

## Notes

- The stress_client **listens** on port 10007 (like a Gateway)
- Engine **connects** to the stress_client
- Multiple sender threads share the same Engine connection
- Requests are sent in open loop (not waiting for responses before sending next request)
- Statistics are collected at both client and server side
- No actual Gateway binary is needed - stress_client replaces it
