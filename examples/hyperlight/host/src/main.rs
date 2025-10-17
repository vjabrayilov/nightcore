/*
Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>
*/

use hyperlight_host::sandbox::{Callable, UninitializedSandbox};
use hyperlight_host::sandbox_state::sandbox::EvolvableSandbox;
use hyperlight_host::sandbox_state::transition::Noop;
use std::env;

fn main() {
    let path = env::var("HYPERLIGHT_GUEST_BIN_PATH")
        .or_else(|_| {
            option_env!("HYPERLIGHT_GUEST_BIN_PATH")
                .map(|s| s.to_string())
                .ok_or_else(|| env::VarError::NotPresent)
        })
        .expect("HYPERLIGHT_GUEST_BIN_PATH not set; set it or use Makefile run target");

    let sandbox = UninitializedSandbox::new(
        hyperlight_host::GuestBinary::FilePath(path.to_string()),
        None,
    )
    .unwrap();

    let mut sbox = sandbox.evolve(Noop::default()).unwrap().new_call_context();

    let guest_result: String = sbox.call("Echo", "hello".to_string()).unwrap();
    println!("guest_result: {}", guest_result);
}
