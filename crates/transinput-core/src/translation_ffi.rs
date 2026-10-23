// SPDX-License-Identifier: AGPL-3.0-only
//! Scheduler-only ABI. No network, credentials, host objects or conversations.
use crate::{
    ffi::{ABI_VERSION, INTERNAL_ERROR, INVALID_ARGUMENT, INVALID_HANDLE, boundary},
    translation::{Context, Failure, Job, MAX_REPLY_BYTES, SYSTEM_PROMPT, Scheduler, Status},
};
use std::{
    collections::HashMap,
    panic::catch_unwind,
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
};
const SOURCE_CAPACITY: usize = 800;
static ENTRIES: OnceLock<Mutex<HashMap<u64, Arc<Entry>>>> = OnceLock::new();
static NEXT: AtomicU64 = AtomicU64::new(1);
static LIVE: AtomicUsize = AtomicUsize::new(0);
struct Entry {
    scheduler: Mutex<Scheduler>,
}
impl Drop for Entry {
    fn drop(&mut self) {
        LIVE.fetch_sub(1, Ordering::Relaxed);
    }
}
fn registry() -> &'static Mutex<HashMap<u64, Arc<Entry>>> {
    ENTRIES.get_or_init(Mutex::default)
}
fn entry(handle: u64) -> Result<Arc<Entry>, i32> {
    registry()
        .lock()
        .map_err(|_| INTERNAL_ERROR)?
        .get(&handle)
        .cloned()
        .ok_or(INVALID_HANDLE)
}
pub(crate) fn shutdown_check() -> Result<(), i32> {
    let mut entries = registry().lock().map_err(|_| INTERNAL_ERROR)?;
    if !entries.is_empty() || LIVE.load(Ordering::Relaxed) != 0 {
        return Err(-4);
    }
    entries.shrink_to_fit();
    Ok(())
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TranslationState {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: u32,
    pub failure: u32,
    pub job: u64,
    pub session: u64,
    pub revision: u64,
    pub configuration: u64,
    pub active_job: u64,
    pub cancel_active: u32,
    pub source_length: u32,
    pub output_length: u32,
    pub reserved: u32,
    pub source: [u8; SOURCE_CAPACITY],
    pub output: [u8; MAX_REPLY_BYTES],
}
fn encode_failure(failure: Failure) -> u32 {
    match failure {
        Failure::Network => 1,
        Failure::Timeout => 2,
        Failure::Authentication => 3,
        Failure::RateLimited => 4,
        Failure::InvalidResponse => 5,
    }
}
fn decode_failure(code: u32) -> Result<Failure, i32> {
    match code {
        1 => Ok(Failure::Network),
        2 => Ok(Failure::Timeout),
        3 => Ok(Failure::Authentication),
        4 => Ok(Failure::RateLimited),
        5 => Ok(Failure::InvalidResponse),
        _ => Err(INVALID_ARGUMENT),
    }
}
fn snapshot(scheduler: &Scheduler, job: Option<Job>) -> TranslationState {
    let context = scheduler.current_context().unwrap_or(Context {
        session: 0,
        revision: 0,
    });
    let (status, failure) = match scheduler.status() {
        Status::Disabled => (0, 0),
        Status::Idle => (1, 0),
        Status::Waiting => (2, 0),
        Status::Running => (3, 0),
        Status::Ready => (4, 0),
        Status::Failed(f) => (5, encode_failure(f)),
    };
    let mut out = TranslationState {
        abi_version: ABI_VERSION,
        struct_size: std::mem::size_of::<TranslationState>() as u32,
        status,
        failure,
        job: 0,
        session: context.session,
        revision: context.revision,
        configuration: scheduler.configuration_id(),
        active_job: scheduler.active_id(),
        cancel_active: u32::from(scheduler.cancellation_requested()),
        source_length: 0,
        output_length: 0,
        reserved: 0,
        source: [0; SOURCE_CAPACITY],
        output: [0; MAX_REPLY_BYTES],
    };
    if let Some(job) = job {
        out.job = job.id;
        out.source_length = job.source().len() as u32;
        out.source[..job.source().len()].copy_from_slice(job.source().as_bytes());
    }
    if let Some(text) = scheduler.visible() {
        out.output_length = text.len() as u32;
        out.output[..text.len()].copy_from_slice(text.as_bytes());
    }
    out
}
fn output_ok(output: *mut TranslationState, size: u32) -> Result<(), i32> {
    if output.is_null() || size < std::mem::size_of::<TranslationState>() as u32 {
        Err(INVALID_ARGUMENT)
    } else {
        Ok(())
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_translator_create() -> u64 {
    catch_unwind(|| {
        let mut entries = registry().lock().ok()?;
        if entries.len() >= 16 {
            return None;
        }
        let id = NEXT.load(Ordering::Relaxed);
        NEXT.store(id.checked_add(1)?, Ordering::Relaxed);
        let value = Arc::new(Entry {
            scheduler: Mutex::new(Scheduler::default()),
        });
        LIVE.fetch_add(1, Ordering::Relaxed);
        entries.insert(id, value);
        Some(id)
    })
    .ok()
    .flatten()
    .unwrap_or(0)
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_translator_destroy(handle: u64) -> i32 {
    boundary(|| {
        registry()
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .remove(&handle)
            .ok_or(INVALID_HANDLE)?;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_translator_configure(handle: u64, configuration: u64, enabled: u32) -> i32 {
    boundary(|| {
        if enabled > 1 {
            return Err(INVALID_ARGUMENT);
        }
        entry(handle)?
            .scheduler
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .configure(configuration, enabled != 0);
        Ok(())
    })
}
/// # Safety
/// Input covers length readable bytes; null is permitted only for zero length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_translator_observe(
    handle: u64,
    session: u64,
    revision: u64,
    input: *const u8,
    length: u32,
    now_ms: u64,
) -> i32 {
    boundary(|| {
        if length as usize > SOURCE_CAPACITY || (length > 0 && input.is_null()) {
            return Err(INVALID_ARGUMENT);
        }
        let bytes = if length == 0 {
            &[][..]
        } else {
            // SAFETY: caller owns length readable bytes; null and capacity were checked.
            unsafe { std::slice::from_raw_parts(input, length as usize) }
        };
        let source = std::str::from_utf8(bytes).map_err(|_| INVALID_ARGUMENT)?;
        entry(handle)?
            .scheduler
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .observe(Context { session, revision }, source, now_ms);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_translator_clear(handle: u64, session: u64, revision: u64) -> i32 {
    boundary(|| {
        entry(handle)?
            .scheduler
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .clear_if_current(Context { session, revision });
        Ok(())
    })
}
/// # Safety
/// Output covers size writable bytes; unaligned buffers are permitted.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_translator_poll(
    handle: u64,
    now_ms: u64,
    output: *mut TranslationState,
    size: u32,
) -> i32 {
    boundary(|| {
        output_ok(output, size)?;
        let value = entry(handle)?;
        let mut scheduler = value.scheduler.lock().map_err(|_| INTERNAL_ERROR)?;
        let job = scheduler.poll(now_ms);
        let out = snapshot(&scheduler, job);
        // SAFETY: caller provides writable storage; size and null were checked.
        unsafe { output.write_unaligned(out) };
        Ok(())
    })
}
/// # Safety
/// Output covers size writable bytes; this getter never consumes a job.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_translator_state(
    handle: u64,
    output: *mut TranslationState,
    size: u32,
) -> i32 {
    boundary(|| {
        output_ok(output, size)?;
        let value = entry(handle)?;
        let scheduler = value.scheduler.lock().map_err(|_| INTERNAL_ERROR)?;
        let out = snapshot(&scheduler, None);
        // SAFETY: caller provides writable storage; size and null were checked.
        unsafe { output.write_unaligned(out) };
        Ok(())
    })
}
/// # Safety
/// On success, input covers length readable bytes; on failure it is ignored.
/// Call only after the platform transport and its callbacks have actually ended.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_translator_finish(
    handle: u64,
    job: u64,
    input: *const u8,
    length: u32,
    failure: u32,
) -> i32 {
    boundary(|| {
        let result = if failure != 0 {
            Err(decode_failure(failure)?)
        } else {
            if length as usize > MAX_REPLY_BYTES || (length > 0 && input.is_null()) {
                return Err(INVALID_ARGUMENT);
            }
            let bytes = if length == 0 {
                &[][..]
            } else {
                // SAFETY: caller owns length readable bytes; length and null were checked.
                unsafe { std::slice::from_raw_parts(input, length as usize) }
            };
            Ok(std::str::from_utf8(bytes)
                .map_err(|_| INVALID_ARGUMENT)?
                .to_owned())
        };
        entry(handle)?
            .scheduler
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .finish(job, result);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_translator_retry(handle: u64, now_ms: u64) -> i32 {
    boundary(|| {
        entry(handle)?
            .scheduler
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .retry(now_ms);
        Ok(())
    })
}
/// # Safety
/// If non-null and capacity is sufficient, output covers capacity writable bytes.
/// Returns required UTF-8 bytes, without a NUL terminator, and never truncates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_translation_prompt(output: *mut u8, capacity: u32) -> u32 {
    let length = SYSTEM_PROMPT.len() as u32;
    if !output.is_null() && capacity >= length {
        // SAFETY: caller owns sufficient writable bytes; no reference is retained.
        unsafe { std::ptr::copy_nonoverlapping(SYSTEM_PROMPT.as_ptr(), output, length as usize) };
    }
    length
}

#[cfg(test)]
mod tests {
    use super::*;
    const SIZE: u32 = std::mem::size_of::<TranslationState>() as u32;
    fn state(handle: u64, poll: Option<u64>) -> TranslationState {
        let mut storage = std::mem::MaybeUninit::uninit();
        // SAFETY: owned, correctly sized writable storage.
        let status = unsafe {
            match poll {
                Some(now) => ti_translator_poll(handle, now, storage.as_mut_ptr(), SIZE),
                None => ti_translator_state(handle, storage.as_mut_ptr(), SIZE),
            }
        };
        assert_eq!(status, 0);
        // SAFETY: successful getter initialized storage.
        unsafe { storage.assume_init() }
    }
    fn observe(handle: u64, session: u64, revision: u64, text: &str, now: u64) {
        assert_eq!(
            // SAFETY: the string owns length readable bytes for this call.
            unsafe {
                ti_translator_observe(
                    handle,
                    session,
                    revision,
                    text.as_ptr(),
                    text.len() as u32,
                    now,
                )
            },
            0
        );
    }
    #[test]
    fn abi_is_transactional_owned_and_rejects_stale_contexts() {
        assert_eq!(SIZE, 4968);
        let handle = ti_translator_create();
        assert_ne!(handle, 0);
        assert_eq!(state(handle, None).status, 0);
        assert_eq!(ti_translator_configure(handle, 1, 2), INVALID_ARGUMENT);
        assert_eq!(ti_translator_configure(handle, 1, 1), 0);
        observe(handle, 9, 1, "你好", 0);
        assert_eq!(state(handle, None).job, 0); // getter must not consume work
        assert_eq!(
            // SAFETY: deliberately invalid output is rejected before polling.
            unsafe { ti_translator_poll(handle, 160, std::ptr::null_mut(), 0) },
            INVALID_ARGUMENT
        );
        let first = state(handle, Some(160));
        assert_ne!(first.job, 0);
        assert_eq!(
            &first.source[..first.source_length as usize],
            "你好".as_bytes()
        );
        observe(handle, 10, 1, "北京", 200);
        assert_eq!(ti_translator_clear(handle, 9, 1), 0);
        assert_eq!(state(handle, None).session, 10);
        assert_eq!(state(handle, None).cancel_active, 1);
        assert_eq!(
            // SAFETY: invalid UTF-8 completion is rejected without freeing the active slot.
            unsafe { ti_translator_finish(handle, first.job, [255u8].as_ptr(), 1, 0) },
            INVALID_ARGUMENT
        );
        assert_eq!(state(handle, Some(500)).active_job, first.job);
        assert_eq!(
            // SAFETY: failure completion ignores input and ends the old transport.
            unsafe { ti_translator_finish(handle, first.job, std::ptr::null(), 0, 1) },
            0
        );
        let second = state(handle, Some(500));
        assert_ne!(second.job, 0);
        assert_eq!(
            // SAFETY: valid reply storage lives for this call.
            unsafe { ti_translator_finish(handle, second.job, b"Beijing".as_ptr(), 7, 0) },
            0
        );
        let captured = state(handle, None);
        assert_eq!(captured.status, 4);
        let mut bytes = vec![0xa5; SIZE as usize + 8];
        let offset = (0..8)
            .find(|i| (bytes.as_ptr() as usize + i) % 8 == 1)
            .unwrap();
        let output = bytes[offset..].as_mut_ptr().cast::<TranslationState>();
        // SAFETY: unaligned output is explicitly permitted and has SIZE bytes.
        assert_eq!(unsafe { ti_translator_state(handle, output, SIZE) }, 0);
        assert_eq!(bytes[offset + SIZE as usize], 0xa5);
        assert_eq!(ti_translator_destroy(handle), 0);
        assert_eq!(ti_translator_destroy(handle), INVALID_HANDLE);
        assert_eq!(
            &captured.output[..captured.output_length as usize],
            b"Beijing"
        );
    }
    #[test]
    fn prompt_size_query_never_truncates() {
        let mut bytes = vec![0xa5; SYSTEM_PROMPT.len()];
        // SAFETY: null size query and owned buffers are permitted.
        unsafe {
            assert_eq!(
                ti_translation_prompt(std::ptr::null_mut(), 0),
                SYSTEM_PROMPT.len() as u32
            );
            ti_translation_prompt(bytes.as_mut_ptr(), 1);
            assert!(bytes.iter().all(|b| *b == 0xa5));
            ti_translation_prompt(bytes.as_mut_ptr(), bytes.len() as u32);
        }
        assert_eq!(bytes, SYSTEM_PROMPT.as_bytes());
    }
}
