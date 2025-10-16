use core::ffi::c_void;
use libc::{size_t, c_char};
use std::ffi::{CStr};
use std::ptr;

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
}

static PREFIX: &str = "From function Bar: ";

#[no_mangle]
pub extern "C" fn faas_init() -> i32 { 0 }

#[no_mangle]
pub extern "C" fn faas_create_func_worker(
    caller_context: *mut c_void,
    invoke_func_fn: InvokeFuncFn,
    append_output_fn: AppendOutputFn,
    worker_handle: *mut *mut c_void,
) -> i32 {
    let ctx = Box::new(WorkerContext {
        caller_ctx: caller_context,
        invoke_fn: Some(invoke_func_fn),
        append_fn: Some(append_output_fn),
    });
    unsafe { *worker_handle = Box::into_raw(ctx) as *mut c_void; }
    0
}

#[no_mangle]
pub extern "C" fn faas_destroy_func_worker(worker_handle: *mut c_void) -> i32 {
    if worker_handle.is_null() { return 0; }
    unsafe { drop(Box::from_raw(worker_handle as *mut WorkerContext)); }
    0
}

#[no_mangle]
pub extern "C" fn faas_func_call(
    worker_handle: *mut c_void,
    input: *const u8,
    input_length: size_t,
) -> i32 {
    if worker_handle.is_null() { return -1; }
    let ctx = unsafe { &mut *(worker_handle as *mut WorkerContext) };

    // call Bar("input")
    let bar = std::ffi::CString::new("Bar").unwrap();
    let mut out_ptr: *const u8 = ptr::null();
    let mut out_len: size_t = 0;
    let ret = (ctx.invoke_fn.unwrap())(
        ctx.caller_ctx,
        bar.as_ptr(),
        input,
        input_length,
        &mut out_ptr as *mut *const u8,
        &mut out_len as *mut size_t,
    );
    if ret != 0 { return -1; }

    // prepend prefix and append output
    let mut buf = Vec::with_capacity(PREFIX.len() + out_len as usize);
    buf.extend_from_slice(PREFIX.as_bytes());
    if out_len > 0 && !out_ptr.is_null() {
        let slice = unsafe { std::slice::from_raw_parts(out_ptr, out_len as usize) };
        buf.extend_from_slice(slice);
    }
    (ctx.append_fn.unwrap())(ctx.caller_ctx, buf.as_ptr(), buf.len() as size_t);
    0
}


