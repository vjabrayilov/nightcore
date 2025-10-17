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

## License and Attribution
Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>
Licensed under Apache-2.0.
