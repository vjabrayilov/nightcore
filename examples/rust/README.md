Example of functions implemented in Rust
==================================

In this example, we define two functions `Foo` and `Bar` in `func_config.json`.
Both functions are implemented in Rust as `cdylib`s and loaded by Nightcore's C++ v1 worker host (`func_worker_v1`).
`Foo` will invoke `Bar` via the provided invoke callback.

Requirements:
- Rust toolchain (1.70+ recommended)

Build:
```
./compile.sh
```

Run the stack:
```
./run_stack.sh
```

Invoke:
```
curl -X POST -d "Hello" http://127.0.0.1:8080/function/Foo
```

Expected output:
```
From function Bar: Hello, World
```


