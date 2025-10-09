# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Nightcore is a research FaaS (Function-as-a-Service) runtime designed for μs-scale latency and high throughput for stateless microservices. Supports C/C++, Go, Node.js, and Python functions.

Published in ASPLOS '21: "Nightcore: Efficient and Scalable Serverless Computing for Latency-Sensitive, Interactive Microservices"

## Build System

### Building from scratch
```bash
./build_deps.sh
make -j $(nproc)
```

### Build configuration
- Main Makefile controls C++ compilation
- Default: release build in `build/release/`, binaries in `bin/release/`
- Debug build: Set `DEBUG_BUILD=1` in `config.mk` or environment
- Compiler: Uses g++ by default (set `CXX` to override)
- Optional `config.mk` can override build settings like `DISABLE_STAT`, `USE_NEW_STAT_COLLECTOR`, `DEBUG_BUILD`, `BUILD_BENCH`

### Dependencies
- Built and installed to `deps/out/` via `build_deps.sh`
- Core deps: abseil-cpp, libuv, http-parser, nghttp2
- Header-only deps: fmt, GSL, nlohmann/json

### Language-specific workers
- **Node.js**: `worker/nodejs/` - Native addon using node-addon-api and N-API
- **Python**: `worker/python/` - Native module, requires Python 3.7+, build with `make` in worker dir
- **Go**: `worker/golang/` - Go module with cgo bindings
- **C/C++**: Uses `func_worker_v1` binary with `.so` files

## Architecture

### Three-Component Runtime Model
Nightcore runs as three separate processes that work together:

1. **Gateway** (`bin/release/gateway`)
   - Entry point for external requests (HTTP on port 8080, gRPC on port 50051)
   - Connects to Engine on port 10007
   - Routes function calls to the Engine
   - Defined in `src/gateway/`, main entry: `src/bin/gateway.cpp`

2. **Engine** (`bin/release/engine`)
   - Core orchestrator and scheduler
   - Manages function dispatching, worker lifecycle, and inter-function calls
   - Communicates with Gateway and Launchers
   - Contains Dispatcher (per-function request scheduler), WorkerManager, Monitor, and Tracer
   - Defined in `src/engine/`, main entry: `src/bin/engine.cpp`

3. **Launcher** (`bin/release/launcher`)
   - One per function type
   - Spawns and manages worker processes for a specific function
   - Bridges Engine with function workers via IPC
   - Supports four modes: cpp, go, nodejs, python (set via `--fprocess_mode`)
   - Defined in `src/launcher/`, main entry: `src/bin/launcher.cpp`

### Communication Architecture
- **Gateway ↔ Engine**: TCP connection (default port 10007)
- **Engine ↔ Launcher**: Unix domain socket or TCP (configurable via `--engine_tcp_port`)
- **Launcher ↔ Workers**: Shared memory and FIFOs for high-performance IPC
  - Input/output FIFOs per worker
  - Shared memory regions for function call data
  - Paths managed in `src/ipc/base.h`

### Function Configuration
Each deployment needs a `func_config.json` with function metadata:
```json
[
  { "funcName": "Foo", "funcId": 1, "minWorkers": 2, "maxWorkers": 2 },
  { "funcName": "Bar", "funcId": 2, "minWorkers": 2, "maxWorkers": 2 }
]
```

### Function API (Worker V1 Interface)
Function libraries must implement the interface defined in `include/faas/worker_v1_interface.h`:
- `faas_init()`: Initialize function library
- `faas_create_func_worker()`: Create worker instance
- `faas_destroy_func_worker()`: Cleanup worker
- `faas_func_call()`: Execute function with input, produce output via callback
- Workers can invoke other functions via `invoke_func_fn` callback

## Source Structure

- `src/base/`: Core utilities, initialization, threading primitives
- `src/common/`: Shared types (protocol definitions, func_config, stats)
- `src/ipc/`: Low-level IPC primitives (shared memory regions, FIFO, SPSC queues)
- `src/engine/`: Engine implementation (dispatcher, worker_manager, message routing)
- `src/gateway/`: Gateway implementation (HTTP/gRPC servers, engine connection)
- `src/launcher/`: Launcher implementation (function process management)
- `src/worker/`: Worker library support code
- `src/server/`: Base server framework (IO workers, connection handling)
- `src/utils/`: Helper utilities (Docker, filesystem, buffer pools)
- `src/bin/`: Binary entry points (`engine.cpp`, `gateway.cpp`, `launcher.cpp`, benchmarks)

- `include/faas/`: Public API headers for function implementations
- `worker/`: Language-specific worker runtime implementations
- `examples/`: Working examples for each supported language

## Running Examples

Each example in `examples/{c,go,node,python}/` follows the same pattern:

1. Compile/prepare function code (e.g., `compile.sh` for C, or just source files for interpreted languages)
2. Run `run_stack.sh` which starts Gateway → Engine → Launcher(s) in sequence
3. Invoke via HTTP: `curl -X POST -d "Hello" http://127.0.0.1:8080/function/Foo`

The examples demonstrate internal function calls (Foo calls Bar) and basic I/O.

## Key Protocols

- `FuncCall` union in `src/common/protocol.h`: Encodes func_id, method_id, client_id, and call_id into 64-bit identifier
- Protocol messages for Gateway ↔ Engine and Engine ↔ Launcher defined in `src/common/protocol.h`
- Shared memory naming and FIFO paths are systematically generated based on client_id and full_call_id

## Function Invocation Flow

### External HTTP Request → Function Execution

1. **HTTP Request arrives at Gateway** (`src/gateway/http_connection.cpp`)
   - Gateway receives HTTP POST to `/function/Foo`
   - Parses function name from URL
   - Calls `Server::OnNewHttpFuncCall()` with function name and input data

2. **Gateway routes to Engine** (`src/gateway/server.cpp:138`)
   - Looks up function in `func_config` by name → gets `func_id`
   - Creates `FuncCall` struct with unique call_id
   - Sends function call message to Engine via TCP connection

3. **Engine Dispatcher schedules execution** (`src/engine/dispatcher.cpp:87`)
   - Engine routes call to appropriate `Dispatcher` (one per function type)
   - Dispatcher checks for idle workers
   - If worker available: dispatches immediately via `DispatchFuncCall()`
   - If no worker: queues in `pending_func_calls_` queue

4. **Launcher spawns Worker process** (if not already running)
   - Launcher starts `func_worker_v1` binary (for C/C++)
   - Or starts language-specific runtime (Node.js/Python/Go worker)
   - Worker connects to Engine via Unix socket or TCP

5. **Worker loads function library** (`src/worker/v1/func_worker.cpp:47`)
   - Worker uses `dlopen()` to load `.so` file (C/C++)
   - Calls `faas_init()` to initialize library
   - Calls `faas_create_func_worker()` to create worker instance
   - Handshakes with Engine, receives FIFO paths for IPC

6. **Worker executes function** (`src/worker/v1/func_worker.cpp:140`)
   - Receives dispatch message from Engine via FIFO/socket
   - Reads input from shared memory region (or inline in message)
   - Calls user's `faas_func_call()` implementation with input
   - User code appends output via callback `append_output_fn`
   - Writes output to shared memory, sends completion message to Engine

7. **Engine returns result to Gateway** (`src/engine/dispatcher.cpp:116`)
   - Dispatcher receives completion from worker
   - Marks worker as idle (or dispatches next queued call)
   - Forwards result to Gateway via TCP

8. **Gateway sends HTTP response**
   - Gateway receives result from Engine
   - Constructs HTTP response with function output
   - Sends response to client

### Internal Function Calls (Foo calls Bar)

When a function invokes another function (e.g., Foo calls Bar):

1. **User code calls invoke_func callback** (`examples/c/foo.c:39`)
   ```c
   context->invoke_func_fn(context->caller_context, "Bar", input, input_length,
                          &bar_output, &bar_output_length);
   ```

2. **Worker sends invoke message to Engine** (`src/worker/v1/func_worker.cpp:177`)
   - `InvokeFuncWrapper()` → `InvokeFunc()`
   - Creates new `FuncCall` with nested call_id
   - Sends invoke message to Engine via output FIFO
   - **Blocks waiting for response** on input FIFO

3. **Engine dispatches nested call**
   - Engine routes to Bar's Dispatcher
   - Bar's Dispatcher assigns to idle Bar worker (same flow as step 3-6 above)
   - Bar worker executes, returns output

4. **Engine returns nested result**
   - Engine sends completion message back to waiting Foo worker
   - Foo worker receives Bar's output from shared memory
   - Foo worker resumes execution with Bar's output

### Function Registration Pattern

All language runtimes follow the same pattern:

**C/C++** (`examples/c/foo.c`):
- Implement 4 functions: `faas_init()`, `faas_create_func_worker()`, `faas_destroy_func_worker()`, `faas_func_call()`
- Compile to `.so`, load via `func_worker_v1 libfoo.so`

**Python** (`examples/python/main.py`):
- Define handler functions: `foo_handler(ctx, input)`, `bar_handler(ctx, input)`
- Return handler from factory: `faas.serve_forever(handler_factory)`
- Worker runtime calls appropriate handler based on `func_name`

**Node.js** (`examples/node/main.js`):
- Define handler functions: `fooHandler(context, input, cb)`, `barHandler(context, input, cb)`
- Register via factory: `faas.serveForever(function (funcName) { return handler })`
- Native addon bridges to C++ worker interface

**Go**: Similar pattern with Go-specific bindings

The key insight: **Function name is resolved to handler at worker startup**, not per-invocation. Each worker process handles exactly one function type.

## Execution Model: Blocking vs Event-Driven

Nightcore supports **two execution modes** depending on the language runtime:

### Mode 1: Blocking/Synchronous (C/C++, default for all languages)

**Worker execution flow** (`src/worker/v1/func_worker.cpp:87-111`):
```cpp
while (true) {
    Message message;
    RecvMessage(input_pipe_fd_, &message);  // BLOCKING read
    if (IsDispatchFuncCallMessage(message)) {
        ExecuteFunc(message);  // BLOCKING execution
    }
}
```

**Key characteristics:**
- Worker has **one execution thread** that blocks waiting for messages
- Function **runs to completion** before accepting next request
- When function calls `invoke_func()` to invoke another function:
  - Worker sends message to Engine
  - Worker **blocks** waiting for response (`RecvMessage` at line 220)
  - Engine dispatches nested call to another worker
  - Original worker remains blocked until nested call completes
  - Only then does function resume execution

**Critical constraint** (`src/worker/v1/func_worker.cpp:210-213`):
```cpp
if (ongoing_invoke_func_) {
    LOG(FATAL) << "NaiveWaitInvokeFunc cannot execute concurrently";
}
```
- **Only ONE nested call at a time** - worker cannot make concurrent invoke_func calls
- Worker cannot handle new requests while executing a function
- **Run-to-completion semantics**: Function must fully complete before worker becomes idle

### Mode 2: Event-Driven/Async (Python with async/await, Node.js)

**Python async runtime** (`worker/python/faas/__init__.py:137-141`):
```python
def on_incoming_func_call(self, handle, method, input_):
    if asyncio.iscoroutinefunction(self._handler):
        self._run_handler_async(handle, method, input_)  # Non-blocking
    else:
        self._run_handler_sync(handle, method, input_)   # Blocking
```

**Key characteristics:**
- Uses `EventDrivenWorker` (`src/worker/event_driven_worker.h`)
- Integrates with language runtime's event loop (asyncio for Python, libuv for Node.js)
- Function invocations **do NOT block** the worker process
- When `async def foo_handler()` calls `await ctx.invoke_func('Bar')`:
  - Creates a Future/Promise
  - Sends invoke message to Engine
  - **Yields control back to event loop** (does not block)
  - Worker can handle other FD events (including other function completions)
  - When nested call completes, Future resolves and coroutine resumes

**Concurrency model:**
- Worker can have **multiple concurrent function executions** in flight
- Each execution is a coroutine/task in the event loop
- Worker continues processing events while waiting for nested calls
- **Cooperative multitasking** within a single process

### Comparison

| Aspect | Blocking (C/C++) | Event-Driven (Python async) |
|--------|------------------|------------------------------|
| **Execution** | Run-to-completion, blocks worker | Async, yields to event loop |
| **Nested calls** | ONE at a time, blocks | Multiple concurrent awaits |
| **Concurrency** | One function execution per worker | Multiple coroutines per worker |
| **Worker state** | Idle or busy (binary) | Event loop always responsive |
| **Implementation** | Simple synchronous loop | Event loop + callbacks |

### Does function run to completion?

**C/C++ mode**: **YES**
- Function executes from start to finish
- Worker is fully blocked during execution
- Worker only accepts next request after function returns

**Python async mode**: **NO** (in the traditional sense)
- Function can be suspended at `await` points
- Worker event loop continues running
- Multiple function executions can be interleaved
- But each individual function **logically** runs to completion (all awaits resolve)

**Node.js**: Similar to Python async mode (callback-based or Promise-based)

## Development Notes

- Nightcore uses C++17, libuv for async I/O, and Abseil for utilities
- Logging via glog-style macros (LOG, VLOG, CHECK, etc.)
- Statistic collection can be disabled with `-D__FAAS_DISABLE_STAT`
- For production benchmarks, use the separate repo: `ut-osa/nightcore-benchmarks`
