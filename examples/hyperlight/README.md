# Hyperlight Example (Host + Guest)

This example demonstrates building and running a Hyperlight guest and a Rust host harness that launches it.

## Prerequisites
- Rust toolchain (stable)
- KVM support (Linux, feature `kvm` enabled via dependency)

## Project layout
- `guest/`: no_std Hyperlight guest binary.
- `host/`: Rust host that builds the guest and then launches it.

## Build
- By default, `cargo build -r` in this workspace builds only the host crate.
- The host build script builds the guest first with the correct flags and exports the path via `HYPERLIGHT_GUEST_BIN_PATH`.

Build host (which will build guest first):

```bash
cd examples/hyperlight
cargo build -r
```

If you want to build the guest manually:

```bash
cd examples/hyperlight/guest
RUSTFLAGS="-C panic=abort -C code-model=small -C link-args=-eentrypoint" \
cargo build -r --target x86_64-unknown-none
```

You can override defaults used by the host build script:

- `HYPERLIGHT_GUEST_RUSTFLAGS` (defaults to `-C panic=abort -C code-model=small -C link-args=-eentrypoint`)
- `HYPERLIGHT_GUEST_TARGET` (defaults to `x86_64-unknown-none`)

## Run
```bash
./target/release/hyperlight-example-host
```
It loads the guest and calls the `PrintHelloWorld` function.

## Troubleshooting
- Duplicate `panic_impl` when building guest crates: ensure the guest is built with `--target x86_64-unknown-none` and `RUSTFLAGS` as above so it is no_std and does not pull `std`.
- If running clippy on the host only, set `HYPERLIGHT_SKIP_GUEST_BUILD=1`:

```bash
cd examples/hyperlight/host
HYPERLIGHT_SKIP_GUEST_BUILD=1 cargo clippy
```
# Hyperlight Integration with Nightcore

This example demonstrates how to use Hyperlight sandboxed functions with Nightcore's FaaS runtime.

## Architecture

The integration consists of three components:

1. **Guest** (`guest/`): A no_std Rust binary that runs inside the Hyperlight sandbox
   - Compiled for `x86_64-unknown-none`
   - Implements function handlers (e.g., `Echo`)
   - Uses Hyperlight's guest API

2. **Worker** (`worker/`): A Rust library that implements Nightcore's `worker_v1` interface
   - Compiles to `libhyperlight_worker.so` (cdylib)
   - Wraps Hyperlight's host API
   - Manages sandbox lifecycle
   - Bridges Nightcore's C FFI to Hyperlight's Rust API

3. **Nightcore Runtime**: Gateway → Engine → Launcher
   - Launcher loads the worker `.so` library
   - Worker creates and manages Hyperlight sandboxes
   - Each function call is executed in an isolated Hyperlight VM

## How It Works

### Function Call Flow

1. HTTP request arrives at Gateway: `POST /function/Echo` with input data
2. Gateway forwards to Engine
3. Engine dispatches to Launcher (func_id=1)
4. Launcher invokes `func_worker_v1` binary with `libhyperlight_worker.so`
5. Worker's `faas_func_call()` is invoked with raw input bytes
6. Worker calls Hyperlight sandbox's `call("Echo", input)`
7. Hyperlight VM executes guest function in isolation
8. Guest returns output via Hyperlight protocol
9. Worker appends output via Nightcore callback
10. Result flows back: Launcher → Engine → Gateway → HTTP response

### Worker V1 Interface Implementation

The worker library (`worker/src/lib.rs`) implements four required functions:

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

## Building

```bash
cd examples/hyperlight
./compile.sh
```

This builds:
- Guest binary: `guest.bin` (165KB no_std binary)
- Worker library: `libecho.so` (1.6MB shared library with Hyperlight host embedded)

## Running

Start the full Nightcore stack:

```bash
./run_stack.sh
```

This launches:
- Gateway (HTTP on :8080, gRPC on :50051)
- Engine (orchestrator)
- Launcher (manages Echo function workers with Hyperlight sandboxes)

Logs are written to `outputs/` directory.

## Testing

```bash
curl -X POST -d "hello" http://127.0.0.1:8080/function/Echo
```

Expected output: `hello` (Echo guest function returns input unchanged)

## Configuration

### func_config.json

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

### Environment Variables

- `HYPERLIGHT_GUEST_BIN_PATH`: Path to guest binary (set in `run_stack.sh`)

## Extending

### Adding New Guest Functions

1. **Update Guest** (`guest/src/main.rs`):
   ```rust
   fn my_function(call: &FunctionCall) -> Result<Vec<u8>> {
       // Implementation
   }

   #[unsafe(no_mangle)]
   pub extern "C" fn hyperlight_main() {
       let def = GuestFunctionDefinition::new(
           "MyFunction".to_string(),
           Vec::from(&[ParameterType::String]),
           ReturnType::String,
           my_function as usize,
       );
       register_function(def);
   }
   ```

2. **Update func_config.json**:
   ```json
   {
       "funcName": "MyFunction",
       "funcId": 2,
       "minWorkers": 2,
       "maxWorkers": 2
   }
   ```

3. **Update Worker** (if function name routing is needed):
   Currently hardcoded to "Echo" - make configurable via env var or command-line arg

4. **Rebuild**:
   ```bash
   ./compile.sh
   ```

### Supporting Multiple Functions

To support multiple functions in a single guest, you'll need to:

1. Make worker function name configurable (e.g., via env var `HYPERLIGHT_FUNC_NAME`)
2. Register multiple functions in `hyperlight_main()`
3. Create separate launchers for each function with different env vars

## Limitations

- **No nested function calls**: Guest cannot invoke other Nightcore functions (would require extending Hyperlight guest with callbacks)
- **String-based serialization**: Currently assumes UTF-8 string inputs/outputs
- **Single function per worker**: Each worker library instance hardcodes function name

## Performance Considerations

- Hyperlight sandboxes use KVM for isolation (requires `/dev/kvm`)
- Each worker creates one sandbox that is reused
- Overhead: ~10-50μs per call (Hyperlight) + Nightcore overhead
- Memory: Each sandbox VM has fixed memory allocation

## Troubleshooting

**Error: "HYPERLIGHT_GUEST_BIN_PATH environment variable not set"**
- Make sure `run_stack.sh` exports the variable before launching launcher

**Error: "Failed to create sandbox"**
- Check KVM availability: `ls -l /dev/kvm`
- Ensure user has permission to access `/dev/kvm`

**Guest binary not found**
- Run `./compile.sh` first
- Verify `guest.bin` exists in `examples/hyperlight/`

## References

- Nightcore architecture: `CLAUDE.md` in repository root
- Hyperlight documentation: https://github.com/hyperlight-dev/hyperlight
- Worker V1 interface: `include/faas/worker_v1_interface.h`


## License and Attribution
Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>
Licensed under Apache-2.0.
