/*
Copyright (c) 2025 Vahab Jabrayilov <vjabayilov@cs.columbia.edu>
*/
#![no_std]
#![no_main]
extern crate alloc;

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use hyperlight_common::flatbuffer_wrappers::function_call::FunctionCall;
use hyperlight_common::flatbuffer_wrappers::function_types::{
    ParameterType, ParameterValue, ReturnType,
};
use hyperlight_common::flatbuffer_wrappers::guest_error::ErrorCode;
use hyperlight_common::flatbuffer_wrappers::util::get_flatbuffer_result;
use hyperlight_guest::error::{HyperlightGuestError, Result};
use hyperlight_guest_bin::guest_function::definition::GuestFunctionDefinition;
use hyperlight_guest_bin::guest_function::register::register_function;

fn echo(function_call: &FunctionCall) -> Result<Vec<u8>> {
    if let Some(params) = &function_call.parameters {
        if let Some(ParameterValue::String(s)) = params.get(0) {
            // get_flatbuffer_result expects &str
            Ok(get_flatbuffer_result(s.as_str()))
        } else {
            Err(HyperlightGuestError::new(
                ErrorCode::GuestFunctionParameterTypeMismatch,
                "Invalid parameters passed to Echo (expected String)".to_string(),
            ))
        }
    } else {
        Err(HyperlightGuestError::new(
            ErrorCode::GuestFunctionParameterTypeMismatch,
            "No parameters passed to Echo (expected String)".to_string(),
        ))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyperlight_main() {
    let echo_def = GuestFunctionDefinition::new(
        "Echo".to_string(),
        Vec::from(&[ParameterType::String]),
        ReturnType::String,
        echo as usize,
    );
    register_function(echo_def);
}

#[unsafe(no_mangle)]
pub fn guest_dispatch_function(function_call: FunctionCall) -> Result<Vec<u8>> {
    let function_name: String = function_call.function_name.clone();
    Err(HyperlightGuestError::new(
        ErrorCode::GuestFunctionNotFound,
        function_name,
    ))
}
