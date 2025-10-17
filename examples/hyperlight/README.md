# Hyperlight + Nightcore Integration

This example demonstrates how to use Hyperlight sandboxed functions with Nightcore's FaaS runtime. It includes both a standalone Hyperlight example and a full Nightcore integration.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Project Layout](#project-layout)
- [Architecture](#architecture)
- [Quick Start](#quick-start)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Configuration](#configuration)
- [Extending](#extending)
- [Troubleshooting](#troubleshooting)
- [References](#references)

## Prerequisites

- Rust toolchain (stable)
- KVM support (Linux, `/dev/kvm` accessible)
- Nightcore binaries built (for integration mode)

## Project Layout

```
examples/hyperlight/
├── guest/              # no_std Hyperlight guest binary
├── host/               # Standalone Hyperlight host harness
├── worker/             # Nightcore worker_v1 wrapper for Hyperlight
├── Makefile            # Build automation
├── compile.sh          # Build script
├── run_stack.sh        # Launch Nightcore stack
└── func_config.json    # Nightcore function configuration
```

### Component Details

- **`guest/`**: A no_std Rust binary that runs inside the Hyperlight sandbox
  - Compiled for `x86_64-unknown-none`
  - Implements function handlers (e.g., `Echo`)
  - Uses Hyperlight's guest API

- **`host/`**: Standalone example demonstrating Hyperlight usage
  - Directly loads and executes guest binary
  - Good for testing guest functions in isolation

- **`worker/`**: Rust library implementing Nightcore's `worker_v1` interface
  - Compiles to `libhyperlight_worker.so` (cdylib)
  - Wraps Hyperlight's host API
  - Manages sandbox lifecycle
  - Bridges Nightcore's C FFI to Hyperlight's Rust API

## Architecture

The Nightcore integration consists of three layers:

### 1. Nightcore Runtime Layer
```
HTTP Request → Gateway → Engine → Launcher
```

### 2. Worker Layer (This Project)
```
Launcher → func_worker_v1 → libhyperlight_worker.so → Hyperlight Host API
```

### 3. Sandbox Layer
```
Hyperlight Host → KVM-based VM → Guest Binary (guest.bin)
```

### Function Call Flow

1. **HTTP Request** arrives at Gateway: `POST /function/Echo`
2. **Gateway** forwards to Engine with function metadata
3. **Engine** dispatches to Launcher for func_id=1
4. **Launcher** invokes `func_worker_v1` binary with `libhyperlight_worker.so`
5. **Worker** `faas_func_call()` is invoked with raw input bytes
6. **Worker** calls Hyperlight sandbox's `call("Echo", input)`
7. **Hyperlight VM** executes guest function in isolation
8. **Guest** returns output via Hyperlight protocol
9. **Worker** appends output via Nightcore callback
10. **Result** flows back through the stack to HTTP response

### Worker V1 Interface

The worker library (`worker/src/lib.rs`) implements four C FFI functions:

```rust
faas_init()                    // Initialize global config (guest binary path)
faas_create_func_worker()      // Create Hyperlight sandbox for each worker process
faas_destroy_func_worker()     // Cleanup sandbox
faas_func_call()               // Execute function call in sandbox
```

### Key Design Decisions

- **Sandbox Reuse**: Each worker process creates one `MultiUseGuestCallContext` that is reused across multiple function calls
- **Synchronous Execution**: Worker blocks during guest execution (matches C/C++ blocking mode)
- **Environment Configuration**: Guest binary path set via `HYPERLIGHT_GUEST_BIN_PATH` env var
- **Type Safety**: Uses `MultiUseGuestCallContext` concrete type (Callable trait is not dyn-safe)

## Quick Start

### Standalone Hyperlight Example

```bash
cd examples/hyperlight
make host            # Build guest + host
make run             # Run standalone example
```

### Nightcore Integration

```bash
cd examples/hyperlight
make demo            # Build everything and start Nightcore stack
```

In another terminal:
```bash
curl -X POST -d "hello" http://127.0.0.1:8080/function/Echo
```

## Building

### Using Makefile (Recommended)

```bash
cd examples/hyperlight
make              # Build guest + worker, create artifacts (default)
make help         # Display all available targets
```

#### Available Targets

| Target | Description |
|--------|-------------|
| `make` or `make all` | Build guest + worker, create artifacts |
| `make nightcore` | Same as `make all` |
| `make guest` | Build only the guest binary |
| `make worker` | Build only the worker library |
| `make host` | Build standalone host example |
| `make demo` | Build and start Nightcore stack |
| `make run` | Run standalone host example |
| `make clean` | Clean build artifacts (guest.bin, libecho.so) |
| `make clean-all` | Clean everything including outputs/ directory |
| `make print` | Display all build variables |
| `make help` | Show help message |

### Using compile.sh

```bash
./compile.sh
```

### Build Artifacts

After building, you'll have:
- `guest.bin` - Hyperlight guest binary (165KB no_std binary)
- `libecho.so` - Worker library for Nightcore (1.6MB shared library)
- `target/release/hyperlight-example-host` - Standalone host binary

### Manual Build

If you want to build the guest manually:

```bash
cd guest
RUSTFLAGS="-C panic=abort -C code-model=small -C link-args=-eentrypoint" \
cargo build --release --target x86_64-unknown-none
```

Override defaults via environment variables:
- `HYPERLIGHT_GUEST_RUSTFLAGS` - Custom RUSTFLAGS for guest
- `HYPERLIGHT_GUEST_TARGET` - Target triple (default: `x86_64-unknown-none`)
- `CARGO` - Cargo command (default: `cargo`)
- `PROFILE` - Build profile (default: `release`)

## Running

### Standalone Mode

Run the standalone host example:

```bash
make run
# or
./target/release/hyperlight-example-host
```

### Nightcore Integration Mode

#### Using Makefile (Recommended)

```bash
make demo        # Build + start Nightcore stack
```

#### Using run_stack.sh

```bash
./run_stack.sh
```

This launches:
- **Gateway** - HTTP on :8080, gRPC on :50051
- **Engine** - Core orchestrator (node_id=0)
- **Launcher** - Manages Echo function workers with Hyperlight sandboxes

Logs are written to `outputs/` directory:
- `outputs/gateway.log` - Gateway logs
- `outputs/engine.log` - Engine logs
- `outputs/launcher_echo.log` - Launcher logs
- `outputs/Echo_worker_*.stdout` - Worker stdout
- `outputs/Echo_worker_*.stderr` - Worker stderr

#### Stopping the Stack

Press `Ctrl+C` in the terminal running `run_stack.sh`, or:

```bash
pkill -f "nightcore/bin/release"
```

## Testing

### Basic Function Call

```bash
curl -X POST -d "hello" http://127.0.0.1:8080/function/Echo
```

Expected output: `hello`

### Multiple Requests

```bash
for i in {1..10}; do
  curl -X POST -d "request $i" http://127.0.0.1:8080/function/Echo
  echo ""
done
```

### Check Logs

```bash
# View gateway logs
tail -f outputs/gateway.log

# View worker output
tail -f outputs/Echo_worker_0.stdout

# View all logs
tail -f outputs/*.log
```

## Configuration

### func_config.json

Defines functions available in Nightcore:

```json
[
    {
        "funcName": "Echo",
        "funcId": 1,
        "minWorkers": 2,
        "maxWorkers": 2
    }
]
```

- `funcName` - Function name used in HTTP requests
- `funcId` - Unique function identifier
- `minWorkers` / `maxWorkers` - Worker process count

### Environment Variables

- `HYPERLIGHT_GUEST_BIN_PATH` - Path to guest binary (set in `run_stack.sh`)
- `HYPERLIGHT_GUEST_RUSTFLAGS` - Build flags for guest
- `HYPERLIGHT_GUEST_TARGET` - Target triple for guest
- `HYPERLIGHT_SKIP_GUEST_BUILD` - Skip guest build in host build.rs

## Extending

### Adding New Guest Functions

**1. Update Guest** (`guest/src/main.rs`):

```rust
fn my_function(call: &FunctionCall) -> Result<Vec<u8>> {
    // Extract parameter
    if let Some(params) = &call.parameters {
        if let Some(ParameterValue::String(input)) = params.get(0) {
            let result = format!("Processed: {}", input);
            return Ok(get_flatbuffer_result(result.as_str()));
        }
    }
    Err(HyperlightGuestError::new(
        ErrorCode::GuestFunctionParameterTypeMismatch,
        "Invalid parameters".to_string(),
    ))
}

#[unsafe(no_mangle)]
pub extern "C" fn hyperlight_main() {
    // Register Echo
    let echo_def = GuestFunctionDefinition::new(
        "Echo".to_string(),
        Vec::from(&[ParameterType::String]),
        ReturnType::String,
        echo as usize,
    );
    register_function(echo_def);

    // Register new function
    let my_fn_def = GuestFunctionDefinition::new(
        "MyFunction".to_string(),
        Vec::from(&[ParameterType::String]),
        ReturnType::String,
        my_function as usize,
    );
    register_function(my_fn_def);
}
```

**2. Update func_config.json**:

```json
[
    {
        "funcName": "Echo",
        "funcId": 1,
        "minWorkers": 2,
        "maxWorkers": 2
    },
    {
        "funcName": "MyFunction",
        "funcId": 2,
        "minWorkers": 2,
        "maxWorkers": 2
    }
]
```

**3. Update Worker** - Make function name configurable:

Currently hardcoded to "Echo" in `worker/src/lib.rs:97`. Options:
- Add env var `HYPERLIGHT_FUNC_NAME`
- Pass function name via command-line arg
- Create separate worker binaries per function

**4. Rebuild and Test**:

```bash
make clean
make demo

# Test new function
curl -X POST -d "test" http://127.0.0.1:8080/function/MyFunction
```

### Supporting Multiple Functions in Single Guest

To have multiple functions in one guest:

1. **Register all functions** in `hyperlight_main()` (shown above)
2. **Make worker configurable**:
   ```rust
   // In worker/src/lib.rs, faas_init():
   let func_name = std::env::var("HYPERLIGHT_FUNC_NAME")
       .unwrap_or_else(|_| "Echo".to_string());
   ```
3. **Create separate launchers** in `run_stack.sh`:
   ```bash
   HYPERLIGHT_FUNC_NAME=Echo $NIGHTCORE_ROOT/bin/release/launcher \
       --func_id=1 --fprocess_mode=cpp ...

   HYPERLIGHT_FUNC_NAME=MyFunction $NIGHTCORE_ROOT/bin/release/launcher \
       --func_id=2 --fprocess_mode=cpp ...
   ```

### Nested Function Calls

Currently **not supported**. To enable:

1. **Extend guest** to accept `invoke_func` callback
2. **Modify worker** to pass callback to guest via Hyperlight host-call mechanism
3. **Handle blocking** - worker must wait for nested call completion

This requires deeper integration with Hyperlight's host-call API.

## Limitations

- **No nested function calls**: Guest cannot invoke other Nightcore functions (would require extending Hyperlight guest with callbacks)
- **String-based serialization**: Currently assumes UTF-8 string inputs/outputs
- **Single function per worker**: Each worker library instance hardcodes function name
- **KVM required**: Must have `/dev/kvm` access for Hyperlight sandboxes
- **Linux only**: Hyperlight with KVM backend is Linux-specific

## Performance Considerations

- **Hyperlight overhead**: ~10-50μs per call (KVM context switch + guest execution)
- **Nightcore overhead**: ~10-100μs (IPC, scheduling, serialization)
- **Total latency**: Expect low microsecond-scale latency for simple functions
- **Sandbox reuse**: Each worker reuses its sandbox across calls (no startup overhead per request)
- **Memory**: Each Hyperlight sandbox allocates fixed memory (configurable in Hyperlight)
- **Concurrency**: Multiple workers run in parallel, each with its own sandbox

## Troubleshooting

### Build Issues

**Error: Duplicate `panic_impl` when building guest**
- Ensure guest is built with `--target x86_64-unknown-none` and proper RUSTFLAGS
- Guest must be no_std and not pull in std

**Error: Cannot find `MultiUseGuestCallContext`**
- Check Hyperlight version in `Cargo.toml` (should be v0.6.0)
- Run `cargo update` to refresh dependencies

**Warning: Creating a shared reference to mutable static**
- This has been fixed in the worker code using `std::ptr::addr_of!`

### Runtime Issues

**Error: "HYPERLIGHT_GUEST_BIN_PATH environment variable not set"**
- Make sure `run_stack.sh` exports the variable before launching launcher
- Or set it manually: `export HYPERLIGHT_GUEST_BIN_PATH=/path/to/guest.bin`

**Error: "Failed to create sandbox"**
- Check KVM availability: `ls -l /dev/kvm`
- Ensure user has permission: `sudo chmod 666 /dev/kvm` (or add user to `kvm` group)
- Verify KVM is enabled: `lsmod | grep kvm`

**Error: "Guest binary not found"**
- Run `make` or `./compile.sh` first
- Verify `guest.bin` exists: `ls -lh guest.bin`
- Check path in `run_stack.sh` matches actual location

**Connection refused errors**
- Ensure Gateway/Engine/Launcher are running: `ps aux | grep nightcore`
- Check logs in `outputs/` directory for startup errors
- Wait a few seconds after starting stack before sending requests

**Function not found**
- Verify function name in `func_config.json` matches guest registration
- Check `funcId` matches between `func_config.json` and launcher command
- Ensure guest built and registered the function in `hyperlight_main()`

### Development Issues

**Running clippy on host only**
```bash
cd host
HYPERLIGHT_SKIP_GUEST_BUILD=1 cargo clippy
```

**Debugging worker crashes**
```bash
# Check worker stderr
cat outputs/Echo_worker_0.stderr

# Run func_worker_v1 manually with debug output
HYPERLIGHT_GUEST_BIN_PATH=./guest.bin \
  ../../bin/release/func_worker_v1 ./libecho.so
```

**Viewing detailed logs**
```bash
# Increase verbosity in run_stack.sh (change --v=1 to --v=2)
--v=2 2>$BASE_DIR/outputs/gateway.log
```

## References

- [Nightcore Architecture](../../CLAUDE.md) - Full Nightcore architecture documentation
- [Hyperlight Documentation](https://github.com/hyperlight-dev/hyperlight) - Official Hyperlight docs
- [Worker V1 Interface](../../include/faas/worker_v1_interface.h) - C interface definition
- [Nightcore Paper](https://www.usenix.org/conference/asplos21/presentation/nightcore) - ASPLOS'21 publication

## License and Attribution

Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>
Licensed under Apache-2.0.

Hyperlight is developed by Microsoft and licensed under Apache-2.0.
Nightcore is research software from UT Austin.
