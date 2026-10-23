// SPDX-License-Identifier: AGPL-3.0-only
//! Owned, bounded learning ABI. Paths and encryption stay in the platform host.
use crate::{
    ffi::{INTERNAL_ERROR, INVALID_ARGUMENT, INVALID_HANDLE, boundary},
    learning::{Learning, MAX_LEARNING_BYTES},
};
use std::{
    collections::HashMap,
    panic::catch_unwind,
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
};

struct Entry {
    model: Arc<Mutex<Learning>>,
}
impl Drop for Entry {
    fn drop(&mut self) {
        LIVE.fetch_sub(1, Ordering::Relaxed);
    }
}
static ENTRIES: OnceLock<Mutex<HashMap<u64, Arc<Entry>>>> = OnceLock::new();
static NEXT: AtomicU64 = AtomicU64::new(1);
static LIVE: AtomicUsize = AtomicUsize::new(0);
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
#[unsafe(no_mangle)]
pub extern "C" fn ti_learning_create() -> u64 {
    catch_unwind(|| {
        let mut entries = registry().lock().ok()?;
        if LIVE.load(Ordering::Relaxed) >= 16 {
            return None;
        }
        let handle = NEXT.load(Ordering::Relaxed);
        NEXT.store(handle.checked_add(1)?, Ordering::Relaxed);
        let value = Arc::new(Entry {
            model: Arc::new(Mutex::new(Learning::default())),
        });
        LIVE.fetch_add(1, Ordering::Relaxed);
        entries.insert(handle, value);
        Some(handle)
    })
    .ok()
    .flatten()
    .unwrap_or(0)
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_learning_destroy(handle: u64) -> i32 {
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
pub extern "C" fn ti_engine_set_learning(engine: u64, learning: u64) -> i32 {
    boundary(|| {
        let shared = crate::ffi::engine(engine)?;
        let model = if learning == 0 {
            None
        } else {
            Some(entry(learning)?.model.clone())
        };
        shared
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .set_learning(model);
        Ok(())
    })
}

/// # Safety
/// `count` covers four writable bytes; alignment is not required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_learning_count(handle: u64, count: *mut u32) -> i32 {
    boundary(|| {
        if count.is_null() {
            return Err(INVALID_ARGUMENT);
        }
        let value = entry(handle)?;
        let size = value.model.lock().map_err(|_| INTERNAL_ERROR)?.len() as u32;
        // SAFETY: caller owns four writable bytes; null was checked.
        unsafe { count.write_unaligned(size) };
        Ok(())
    })
}

/// # Safety
/// Nonzero-length buffers must contain the declared readable bytes for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_learning_record(
    handle: u64,
    input: *const u8,
    input_size: u32,
    text: *const u8,
    text_size: u32,
) -> i32 {
    // SAFETY: identical readable-buffer contract; zero selects base dictionary only.
    unsafe { ti_learning_record_with_words(handle, 0, input, input_size, text, text_size) }
}

/// Record against the same bounded personal/base candidate set used by queries.
/// Zero words selects base-only validation. No arbitrary text is admitted.
/// # Safety
/// Nonzero-length buffers contain the declared readable bytes for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_learning_record_with_words(
    handle: u64,
    words: u64,
    input: *const u8,
    input_size: u32,
    text: *const u8,
    text_size: u32,
) -> i32 {
    boundary(|| {
        if input.is_null()
            || text.is_null()
            || input_size == 0
            || input_size > 128
            || text_size == 0
            || text_size > 512
        {
            return Err(INVALID_ARGUMENT);
        }
        // SAFETY: caller provides readable buffers; nulls and bounded lengths were checked.
        let input =
            std::str::from_utf8(unsafe { std::slice::from_raw_parts(input, input_size as usize) })
                .map_err(|_| INVALID_ARGUMENT)?;
        // SAFETY: same readable-buffer contract and bounds as above.
        let text =
            std::str::from_utf8(unsafe { std::slice::from_raw_parts(text, text_size as usize) })
                .map_err(|_| INVALID_ARGUMENT)?;
        let value = entry(handle)?;
        let personal = if words == 0 {
            None
        } else {
            Some(crate::user_words_ffi::snapshot(words)?)
        };
        if !crate::ffi::dictionary()?
            .candidates_with_personal(input, personal.as_deref())
            .iter()
            .any(|candidate| candidate == text)
        {
            return Err(INVALID_ARGUMENT);
        }
        if !value
            .model
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .record(input, text)
        {
            return Err(INVALID_ARGUMENT);
        }
        Ok(())
    })
}

/// # Safety
/// `data` covers `size` readable bytes for this call. Decoding is transactional.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_learning_import(handle: u64, data: *const u8, size: u32) -> i32 {
    boundary(|| {
        if data.is_null() || size as usize > MAX_LEARNING_BYTES {
            return Err(INVALID_ARGUMENT);
        }
        let value = entry(handle)?;
        // SAFETY: caller guarantees readable storage, and null/size were checked.
        let bytes = unsafe { std::slice::from_raw_parts(data, size as usize) };
        let model = Learning::from_bytes(bytes).map_err(|_| INVALID_ARGUMENT)?;
        *value.model.lock().map_err(|_| INTERNAL_ERROR)? = model;
        Ok(())
    })
}

/// # Safety
/// `written` covers four writable bytes (alignment is not required). A nonnull
/// `data` covers `capacity` writable bytes. Null + zero capacity queries the size;
/// a concurrent record can change that size, so platform code uses the fixed cap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_learning_export(
    handle: u64,
    data: *mut u8,
    capacity: u32,
    written: *mut u32,
) -> i32 {
    boundary(|| {
        if written.is_null() || (data.is_null() && capacity != 0) {
            return Err(INVALID_ARGUMENT);
        }
        let value = entry(handle)?;
        let bytes = value.model.lock().map_err(|_| INTERNAL_ERROR)?.export();
        if !data.is_null() && (capacity as usize) < bytes.len() {
            return Err(INVALID_ARGUMENT);
        }
        // SAFETY: caller owns four writable bytes, possibly unaligned.
        unsafe { written.write_unaligned(bytes.len() as u32) };
        if !data.is_null() {
            // SAFETY: caller guarantees capacity bytes; capacity and null were checked.
            unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), data, bytes.len()) };
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn personal_records_validate_the_current_profile_and_candidate_budget() {
        let learning = ti_learning_create();
        let words = crate::user_words_ffi::ti_words_create();
        let other = crate::user_words_ffi::ti_words_create();
        let pinyin = b"xing he yan jiu suo";
        let text = "星河研究所".as_bytes();
        let edit = |remove| {
            // SAFETY: the fixed arrays own all declared readable bytes.
            unsafe {
                crate::user_words_ffi::ti_words_edit(
                    words,
                    remove,
                    pinyin.as_ptr(),
                    pinyin.len() as u32,
                    text.as_ptr(),
                    text.len() as u32,
                )
            }
        };
        let record = |profile, input: &str, value: &[u8]| {
            // SAFETY: string/slice own all declared readable bytes.
            unsafe {
                ti_learning_record_with_words(
                    learning,
                    profile,
                    input.as_ptr(),
                    input.len() as u32,
                    value.as_ptr(),
                    value.len() as u32,
                )
            }
        };
        assert_eq!(edit(0), 0);
        for input in [
            "xhyjs",
            "xinghe",
            "xingheyanjiusuo",
            "nihaoxingheyanjiusuo",
            "xinghyjs",
        ] {
            let candidates = crate::ffi::dictionary().unwrap().candidates_with_personal(
                input,
                Some(&crate::user_words_ffi::snapshot(words).unwrap()),
            );
            let chosen = if input.starts_with("nihao") {
                "你好星河研究所".as_bytes()
            } else {
                text
            };
            assert!(candidates.iter().any(|c| c.as_bytes() == chosen));
            assert_eq!(record(words, input, chosen), 0);
        }
        assert_eq!(record(other, "xhyjs", text), INVALID_ARGUMENT);
        assert_eq!(record(words, "n", text), INVALID_ARGUMENT);
        assert_eq!(
            record(words, "xhyjs", "任意虚构注入".as_bytes()),
            INVALID_ARGUMENT
        );
        assert_eq!(edit(1), 0);
        assert_eq!(record(words, "xhyjs", text), INVALID_ARGUMENT);
        assert_eq!(crate::user_words_ffi::ti_words_destroy(words), 0);
        assert_eq!(record(words, "xhyjs", text), INVALID_HANDLE);
        assert_eq!(crate::user_words_ffi::ti_words_destroy(other), 0);
        assert_eq!(ti_learning_destroy(learning), 0);
    }
    #[test]
    fn abi_owns_preferences_and_rejects_invalid_or_stale_updates() {
        let learning = ti_learning_create();
        let engine = crate::ffi::ti_engine_create();
        assert!(learning != 0 && engine != 0);
        let mut count_bytes = [0xa5; 6];
        let count = count_bytes.as_mut_ptr().wrapping_add(1).cast::<u32>();
        // SAFETY: four writable bytes exist at the deliberately unaligned offset.
        assert_eq!(unsafe { ti_learning_count(learning, count) }, 0);
        assert_eq!(count_bytes, [0xa5, 0, 0, 0, 0, 0xa5]);
        assert_eq!(
            // SAFETY: null is deliberately rejected before any access.
            unsafe { ti_learning_count(learning, std::ptr::null_mut()) },
            INVALID_ARGUMENT
        );
        assert_eq!(ti_engine_set_learning(engine, learning), 0);
        let input = b"zg";
        let chinese = "中国".as_bytes();
        assert_eq!(
            // SAFETY: fixed arrays own their declared readable bytes.
            unsafe {
                ti_learning_record(
                    learning,
                    input.as_ptr(),
                    2,
                    chinese.as_ptr(),
                    chinese.len() as u32,
                )
            },
            0
        );
        let mut output = std::mem::MaybeUninit::<crate::ffi::Snapshot>::uninit();
        // SAFETY: same valid four-byte unaligned result buffer.
        assert_eq!(unsafe { ti_learning_count(learning, count) }, 0);
        assert_eq!(u32::from_ne_bytes(count_bytes[1..5].try_into().unwrap()), 1);
        assert_eq!(
            // SAFETY: fixed input and caller-owned Snapshot storage are valid.
            unsafe {
                crate::ffi::ti_engine_query(
                    engine,
                    input.as_ptr(),
                    2,
                    2,
                    output.as_mut_ptr(),
                    crate::ffi::ti_snapshot_size(),
                )
            },
            0
        );
        // SAFETY: successful query initialized the snapshot.
        let snapshot = unsafe { output.assume_init() };
        assert_eq!(&snapshot.candidates[0].bytes[..6], chinese);
        let mut encoded = vec![0; MAX_LEARNING_BYTES];
        let mut size = 0;
        assert_eq!(
            // SAFETY: encoded and size own writable storage of the declared capacities.
            unsafe {
                ti_learning_export(
                    learning,
                    encoded.as_mut_ptr(),
                    encoded.len() as u32,
                    &mut size,
                )
            },
            0
        );
        encoded.truncate(size as usize);
        let before = encoded.clone();
        assert_eq!(
            // SAFETY: invalid but readable fixed data, rejected before replacing preferences.
            unsafe { ti_learning_import(learning, b"invalid".as_ptr(), 7) },
            INVALID_ARGUMENT
        );
        let mut after = vec![0; MAX_LEARNING_BYTES];
        assert_eq!(
            // SAFETY: after and size own the declared writable storage.
            unsafe {
                ti_learning_export(learning, after.as_mut_ptr(), after.len() as u32, &mut size)
            },
            0
        );
        after.truncate(size as usize);
        assert_eq!(after, before);
        let mut undersized = [0xa5; 2];
        let mut unchanged_size = 77;
        assert_eq!(
            // SAFETY: both outputs own valid storage; undersized capacity is intentionally rejected.
            unsafe {
                ti_learning_export(learning, undersized.as_mut_ptr(), 2, &mut unchanged_size)
            },
            INVALID_ARGUMENT
        );
        assert_eq!(undersized, [0xa5; 2]);
        assert_eq!(unchanged_size, 77);
        let mut size_bytes = [0u8; 5];
        assert_eq!(
            // SAFETY: four bytes after the first byte are writable even when unaligned.
            unsafe {
                ti_learning_export(
                    learning,
                    std::ptr::null_mut(),
                    0,
                    size_bytes[1..].as_mut_ptr().cast(),
                )
            },
            0
        );
        assert_eq!(
            u32::from_ne_bytes(size_bytes[1..].try_into().unwrap()) as usize,
            before.len()
        );
        assert_eq!(ti_learning_destroy(learning), 0);
        assert_eq!(ti_learning_destroy(learning), INVALID_HANDLE);
        // The engine keeps its shared preference model alive without a C handle.
        assert_eq!(ti_engine_set_learning(engine, 0), 0);
        assert_eq!(crate::ffi::ti_engine_destroy(engine), 0);
    }
}
