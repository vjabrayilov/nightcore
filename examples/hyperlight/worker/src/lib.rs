/*
Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>

This worker library implements the Nightcore worker_v1 interface for Hyperlight sandboxes.
It allows Hyperlight guest binaries to be used as Nightcore functions.
*/

use core::ffi::c_void;
use libc::{c_char, size_t};
use std::sync::{Arc, Mutex};

use hyperlight_host::sandbox::{Callable, UninitializedSandbox};
use hyperlight_host::sandbox_state::sandbox::EvolvableSandbox;
use hyperlight_host::sandbox_state::transition::Noop;
use hyperlight_host::{GuestBinary, MultiUseGuestCallContext};

// Mirror of include/faas/worker_v1_interface.h
pub type AppendOutputFn = extern "C" fn(*mut c_void, *const u8, size_t);
pub type InvokeFuncFn = extern "C" fn(
    *mut c_void,
    *const c_char,
    *const u8,
    size_t,
    *mut *const u8,
    *mut size_t,
) -> i32;

#[repr(C)]
pub struct WorkerContext {
    caller_ctx: *mut c_void,
    invoke_fn: Option<InvokeFuncFn>,
    append_fn: Option<AppendOutputFn>,
    // Hyperlight sandbox using the concrete type
    sandbox: Arc<Mutex<MultiUseGuestCallContext>>,
    func_name: String,
}

// Global configuration - path to guest binary
static mut GUEST_BINARY_PATH: Option<String> = None;

#[no_mangle]
pub extern "C" fn faas_init() -> i32 {
    // Initialize guest binary path from environment variable
    unsafe {
        if let Ok(path) = std::env::var("HYPERLIGHT_GUEST_BIN_PATH") {
            GUEST_BINARY_PATH = Some(path);
            0
        } else {
            eprintln!("ERROR: HYPERLIGHT_GUEST_BIN_PATH environment variable not set");
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn faas_create_func_worker(
    caller_context: *mut c_void,
    invoke_func_fn: InvokeFuncFn,
    append_output_fn: AppendOutputFn,
    worker_handle: *mut *mut c_void,
) -> i32 {
    unsafe {
        let guest_path = match std::ptr::addr_of!(GUEST_BINARY_PATH).read() {
            Some(ref path) => path.clone(),
            None => {
                eprintln!("ERROR: Guest binary path not initialized");
                return -1;
            }
        };

        // Create Hyperlight sandbox
        let sandbox = match UninitializedSandbox::new(GuestBinary::FilePath(guest_path), None) {
            Ok(sb) => sb,
            Err(e) => {
                eprintln!("ERROR: Failed to create sandbox: {:?}", e);
                return -1;
            }
        };

        let evolved = match sandbox.evolve(Noop::default()) {
            Ok(sb) => sb,
            Err(e) => {
                eprintln!("ERROR: Failed to evolve sandbox: {:?}", e);
                return -1;
            }
        };

        let call_context = evolved.new_call_context();

        let ctx = Box::new(WorkerContext {
            caller_ctx: caller_context,
            invoke_fn: Some(invoke_func_fn),
            append_fn: Some(append_output_fn),
            sandbox: Arc::new(Mutex::new(call_context)),
            func_name: String::from("Echo"), // Default function name, can be made configurable
        });

        *worker_handle = Box::into_raw(ctx) as *mut c_void;
        0
    }
}

#[no_mangle]
pub extern "C" fn faas_destroy_func_worker(worker_handle: *mut c_void) -> i32 {
    if worker_handle.is_null() {
        return 0;
    }
    unsafe {
        drop(Box::from_raw(worker_handle as *mut WorkerContext));
    }
    0
}

#[no_mangle]
pub extern "C" fn faas_func_call(
    worker_handle: *mut c_void,
    input: *const u8,
    input_length: size_t,
) -> i32 {
    if worker_handle.is_null() {
        return -1;
    }

    let ctx = unsafe { &mut *(worker_handle as *mut WorkerContext) };

    // Convert raw input bytes to String for Hyperlight call
    let input_slice = unsafe { std::slice::from_raw_parts(input, input_length as usize) };
    let input_str = match std::str::from_utf8(input_slice) {
        Ok(s) => s.to_string(),
        Err(e) => {
            eprintln!("ERROR: Invalid UTF-8 input: {:?}", e);
            return -1;
        }
    };

    // Call Hyperlight guest function
    let result: Result<String, _> = {
        let mut sandbox = ctx.sandbox.lock().unwrap();
        sandbox.call(&ctx.func_name, input_str)
    };

    match result {
        Ok(output) => {
            // Append output via callback
            let output_bytes = output.as_bytes();
            (ctx.append_fn.unwrap())(
                ctx.caller_ctx,
                output_bytes.as_ptr(),
                output_bytes.len() as size_t,
            );
            0
        }
        Err(e) => {
            eprintln!("ERROR: Guest function call failed: {:?}", e);
            -1
        }
    }
}

// Optional: Support for invoking other functions from within a Hyperlight guest
// This would require extending the guest with a callback mechanism, which is
// more complex and may not be immediately needed. For now, we support simple
// Echo-style functions that don't need to invoke other functions.
