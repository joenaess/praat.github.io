// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2007-2026 Jonas Lindh

//! C-ABI Foreign Function Interface for Praat C++ integration.
//!
//! Every `extern "C"` entrypoint wraps its body in [`std::panic::catch_unwind`]
//! so Rust panics can never unwind across the C++ boundary. Detailed error
//! messages are recorded in thread-local storage retrievable via
//! [`voxalys_last_error`].

use std::cell::RefCell;
use std::ffi::{c_char, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use crate::jovoice_bridge::{VoxalysModelHandle, VoxalysSummaryResult};
use crate::ring_buffer::VoxalysStreamHandle;
use crate::vrp_analyzer::VoxalysVrpPoint;

const VERSION_CSTR: &[u8] = b"voxalys_rust 0.1.0 (JoVoice DSP core)\0";

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap_or_default());
}

fn set_last_error(msg: impl Into<String>) {
    let clean = msg.into().replace('\0', " ");
    let cstr = CString::new(clean).unwrap_or_default();
    LAST_ERROR.with(|cell| {
        *cell.borrow_mut() = cstr;
    });
}

fn clear_last_error() {
    set_last_error("");
}

/// Returns a static NUL-terminated library version string.
#[no_mangle]
pub extern "C" fn voxalys_version() -> *const c_char {
    VERSION_CSTR.as_ptr().cast()
}

/// Returns the last error message on the calling thread (empty string if none).
#[no_mangle]
pub extern "C" fn voxalys_last_error() -> *const c_char {
    LAST_ERROR.with(|cell| cell.borrow().as_ptr())
}

/// Creates a new JoVoice/Voxalys analysis model handle. Returns null on error.
#[no_mangle]
pub extern "C" fn voxalys_model_new(
    f0_min_hz: f32,
    f0_max_hz: f32,
    spl_calibration_offset_db: f32,
) -> *mut VoxalysModelHandle {
    clear_last_error();
    match catch_unwind(|| VoxalysModelHandle::new(f0_min_hz, f0_max_hz, spl_calibration_offset_db))
    {
        Ok(Ok(handle)) => Box::into_raw(Box::new(handle)),
        Ok(Err(err)) => {
            set_last_error(err);
            ptr::null_mut()
        }
        Err(_) => {
            set_last_error("Rust panic in voxalys_model_new");
            ptr::null_mut()
        }
    }
}

/// Frees a handle returned by [`voxalys_model_new`]. Null pointers are safely ignored.
///
/// # Safety
/// `handle` must be null or a pointer previously returned by [`voxalys_model_new`]
/// that has not yet been freed.
#[no_mangle]
pub unsafe extern "C" fn voxalys_model_free(handle: *mut VoxalysModelHandle) {
    if handle.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Caller passes pointer previously returned by `voxalys_model_new`.
        drop(unsafe { Box::from_raw(handle) });
    }));
}

/// Creates a lock-free SPSC audio stream and incremental VRP analyzer.
/// Returns null on error (inspect [`voxalys_last_error`]).
#[no_mangle]
pub extern "C" fn voxalys_stream_new(
    sample_rate: f32,
    capacity_samples: usize,
    f0_min_hz: f32,
    f0_max_hz: f32,
    spl_calibration_offset_db: f32,
) -> *mut VoxalysStreamHandle {
    clear_last_error();
    match catch_unwind(|| {
        VoxalysStreamHandle::new(
            sample_rate,
            capacity_samples,
            f0_min_hz,
            f0_max_hz,
            spl_calibration_offset_db,
        )
    }) {
        Ok(Ok(stream)) => Box::into_raw(Box::new(stream)),
        Ok(Err(err)) => {
            set_last_error(err);
            ptr::null_mut()
        }
        Err(_) => {
            set_last_error("Rust panic in voxalys_stream_new");
            ptr::null_mut()
        }
    }
}

/// Pushes mono `f32` audio samples from the real-time audio callback thread.
///
/// Lock-free and allocation-free. Returns the number of samples accepted.
///
/// # Safety
/// `handle` must be a valid stream pointer returned by [`voxalys_stream_new`],
/// `samples` must be valid for `len` contiguous `f32` reads, and only one
/// producer thread may call this function concurrently.
#[no_mangle]
pub unsafe extern "C" fn voxalys_stream_push_samples(
    handle: *mut VoxalysStreamHandle,
    samples: *const f32,
    len: usize,
) -> usize {
    if handle.is_null() || samples.is_null() || len == 0 {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Non-null pointers validated above; SPSC single-producer contract applies.
        let stream = unsafe { &*handle };
        let slice = unsafe { std::slice::from_raw_parts(samples, len) };
        unsafe { stream.push_samples(slice) }
    }))
    .unwrap_or_default()
}

/// Polls newly analyzed VRP points from the consumer/GUI thread.
///
/// Returns the number of points written to `out_points` (at most `max_points`).
///
/// # Safety
/// `handle` must be a valid stream pointer, `out_points` must be writable for
/// `max_points` elements, and only one consumer thread may call this function.
#[no_mangle]
pub unsafe extern "C" fn voxalys_stream_poll_points(
    handle: *mut VoxalysStreamHandle,
    out_points: *mut VoxalysVrpPoint,
    max_points: usize,
) -> usize {
    if handle.is_null() || out_points.is_null() || max_points == 0 {
        return 0;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Non-null pointers validated above; SPSC single-consumer contract applies.
        let stream = unsafe { &*handle };
        let out_slice = unsafe { std::slice::from_raw_parts_mut(out_points, max_points) };
        unsafe { stream.poll_points(out_slice) }
    })) {
        Ok(n) => n,
        Err(_) => {
            set_last_error("Rust panic in voxalys_stream_poll_points");
            0
        }
    }
}

/// Resets a stream handle's buffered audio and VRP state.
///
/// # Safety
/// `handle` must be null or a valid stream pointer whose producer is stopped.
#[no_mangle]
pub unsafe extern "C" fn voxalys_stream_reset(handle: *mut VoxalysStreamHandle) {
    if handle.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Pointer validated non-null; called on consumer thread when stream is stopped.
        let stream = unsafe { &*handle };
        unsafe { stream.reset() };
    }));
}

/// Returns total dropped audio samples since creation or last reset.
///
/// # Safety
/// `handle` must be null or a valid stream pointer.
#[no_mangle]
pub unsafe extern "C" fn voxalys_stream_dropped_samples(handle: *const VoxalysStreamHandle) -> u64 {
    if handle.is_null() {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Non-null pointer validated above.
        unsafe { &*handle }.dropped_samples()
    }))
    .unwrap_or_default()
}

/// Frees a stream handle returned by [`voxalys_stream_new`].
///
/// # Safety
/// `handle` must be null or a pointer previously returned by [`voxalys_stream_new`]
/// that has not yet been freed, with no active producer or consumer threads.
#[no_mangle]
pub unsafe extern "C" fn voxalys_stream_free(handle: *mut VoxalysStreamHandle) {
    if handle.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Pointer previously returned by `voxalys_stream_new`.
        drop(unsafe { Box::from_raw(handle) });
    }));
}

/// Analyzes an entire mono `f32` audio buffer with the JoVoice/Voxalys engine.
///
/// Returns `0` on success, negative error code on failure (see [`voxalys_last_error`]).
///
/// # Safety
/// `model` and `out_summary` must be valid non-null pointers, and `samples`
/// must be valid for `len` contiguous `f32` elements when `len > 0`.
#[no_mangle]
pub unsafe extern "C" fn voxalys_analyze_buffer(
    model: *const VoxalysModelHandle,
    samples: *const f32,
    len: usize,
    sample_rate: f32,
    out_summary: *mut VoxalysSummaryResult,
) -> i32 {
    clear_last_error();
    if model.is_null() || out_summary.is_null() || (len > 0 && samples.is_null()) {
        set_last_error("Null pointer argument passed to voxalys_analyze_buffer");
        return -1;
    }

    match catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: All pointers checked non-null above.
        let model_ref = unsafe { &*model };
        let slice = if len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(samples, len) }
        };
        model_ref.analyze_buffer(slice, sample_rate)
    })) {
        Ok(Ok((summary, _frames))) => {
            // SAFETY: `out_summary` verified non-null above.
            unsafe {
                *out_summary = summary;
            }
            0
        }
        Ok(Err(err)) => {
            set_last_error(err);
            -2
        }
        Err(_) => {
            set_last_error("Rust panic in voxalys_analyze_buffer");
            -3
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::PI;
    use std::ffi::CStr;

    #[test]
    fn ffi_roundtrip_model_and_stream() {
        let ver = unsafe { CStr::from_ptr(voxalys_version()) }
            .to_str()
            .unwrap();
        assert!(ver.contains("voxalys_rust"));

        let model = voxalys_model_new(65.0, 800.0, 94.0);
        assert!(!model.is_null());

        let sr = 16_000.0f32;
        let samples: Vec<f32> = (0..16_000)
            .map(|i| 0.5 * (2.0 * PI * 150.0 * i as f32 / sr).sin())
            .collect();

        let mut summary = VoxalysSummaryResult::default();
        let rc = unsafe {
            voxalys_analyze_buffer(model, samples.as_ptr(), samples.len(), sr, &mut summary)
        };
        assert_eq!(rc, 0);
        assert!((summary.f0_mean_hz - 150.0).abs() < 1.0);
        assert!(summary.voiced_frame_count > 50);

        unsafe { voxalys_model_free(model) };

        let stream = voxalys_stream_new(sr, 32_768, 65.0, 800.0, 94.0);
        assert!(!stream.is_null());
        let pushed =
            unsafe { voxalys_stream_push_samples(stream, samples.as_ptr(), samples.len()) };
        assert_eq!(pushed, samples.len());

        let mut pts = vec![
            VoxalysVrpPoint {
                time_sec: 0.0,
                f0_hz: 0.0,
                spl_db: 0.0,
                voiced: false,
                quality_score: 0.0,
                phonation_class: crate::jovoice_bridge::VoxalysPhonationClass::Unvoiced,
            };
            256
        ];
        let polled = unsafe { voxalys_stream_poll_points(stream, pts.as_mut_ptr(), pts.len()) };
        assert!(polled > 50);
        assert_eq!(unsafe { voxalys_stream_dropped_samples(stream) }, 0);

        unsafe {
            voxalys_stream_reset(stream);
            voxalys_stream_free(stream);
        }
    }

    #[test]
    fn ffi_handles_invalid_args_without_panicking() {
        assert!(voxalys_model_new(500.0, 100.0, 94.0).is_null());
        let err = unsafe { CStr::from_ptr(voxalys_last_error()) }
            .to_str()
            .unwrap();
        assert!(err.contains("Invalid F0 range"));

        let mut summary = VoxalysSummaryResult::default();
        let rc =
            unsafe { voxalys_analyze_buffer(ptr::null(), ptr::null(), 0, 16000.0, &mut summary) };
        assert_eq!(rc, -1);
    }
}
