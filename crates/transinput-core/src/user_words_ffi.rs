// SPDX-License-Identifier: AGPL-3.0-only
//! Bounded personal-word handles. Platform code owns encryption and persistence.
use crate::{
    ffi::{INTERNAL_ERROR, INVALID_ARGUMENT, INVALID_HANDLE, Text, boundary},
    user_words::UserWords,
};
use std::{
    collections::HashMap,
    panic::catch_unwind,
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
};
pub(crate) type SharedWords = Arc<Mutex<Arc<UserWords>>>;
struct Entry {
    model: SharedWords,
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
pub(crate) fn snapshot(handle: u64) -> Result<Arc<UserWords>, i32> {
    Ok(entry(handle)?
        .model
        .lock()
        .map_err(|_| INTERNAL_ERROR)?
        .clone())
}
fn create(model: Arc<UserWords>) -> u64 {
    catch_unwind(|| {
        let mut values = registry().lock().ok()?;
        if LIVE.load(Ordering::Relaxed) >= 16 {
            return None;
        }
        let handle = NEXT.load(Ordering::Relaxed);
        NEXT.store(handle.checked_add(1)?, Ordering::Relaxed);
        let value = Arc::new(Entry {
            model: Arc::new(Mutex::new(model)),
        });
        LIVE.fetch_add(1, Ordering::Relaxed);
        values.insert(handle, value);
        Some(handle)
    })
    .ok()
    .flatten()
    .unwrap_or(0)
}
pub(crate) fn shutdown_check() -> Result<(), i32> {
    let mut values = registry().lock().map_err(|_| INTERNAL_ERROR)?;
    if !values.is_empty() || LIVE.load(Ordering::Relaxed) != 0 {
        return Err(-4);
    }
    values.shrink_to_fit();
    Ok(())
}
#[repr(C)]
pub struct WordInfo {
    pub abi_version: u32,
    pub struct_size: u32,
    pub revision: u64,
    pub count: u32,
    pub reserved: u32,
}
#[repr(C)]
pub struct WordRow {
    pub abi_version: u32,
    pub struct_size: u32,
    pub pinyin: Text,
    pub text: Text,
}

#[unsafe(no_mangle)]
pub extern "C" fn ti_words_create() -> u64 {
    create(Arc::new(UserWords::default()))
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_words_clone(handle: u64) -> u64 {
    snapshot(handle).map_or(0, create)
}
#[unsafe(no_mangle)]
pub extern "C" fn ti_words_destroy(handle: u64) -> i32 {
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
pub extern "C" fn ti_engine_set_words(engine: u64, handle: u64) -> i32 {
    boundary(|| {
        let model = if handle == 0 {
            None
        } else {
            Some(entry(handle)?.model.clone())
        };
        crate::ffi::engine(engine)?
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .set_user_words(model);
        Ok(())
    })
}
/// Swap a fully prepared immutable snapshot. The platform saves before publishing.
#[unsafe(no_mangle)]
pub extern "C" fn ti_words_publish(destination: u64, prepared: u64) -> i32 {
    boundary(|| {
        let next = snapshot(prepared)?;
        *entry(destination)?
            .model
            .lock()
            .map_err(|_| INTERNAL_ERROR)? = next;
        Ok(())
    })
}
/// # Safety
/// `output` covers `size` writable bytes; alignment is not required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_words_info(handle: u64, output: *mut WordInfo, size: u32) -> i32 {
    boundary(|| {
        if output.is_null() || size as usize != size_of::<WordInfo>() {
            return Err(INVALID_ARGUMENT);
        }
        let value = snapshot(handle)?;
        // SAFETY: caller owns the checked-size output buffer.
        unsafe {
            output.write_unaligned(WordInfo {
                abi_version: 1,
                struct_size: size,
                revision: value.revision(),
                count: value.len() as u32,
                reserved: 0,
            });
        }
        Ok(())
    })
}
/// # Safety
/// `output` covers `size` writable bytes; alignment is not required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_words_get(
    handle: u64,
    index: u32,
    output: *mut WordRow,
    size: u32,
) -> i32 {
    boundary(|| {
        if output.is_null() || size as usize != size_of::<WordRow>() {
            return Err(INVALID_ARGUMENT);
        }
        let value = snapshot(handle)?;
        let row = value.row(index as usize).ok_or(INVALID_ARGUMENT)?;
        // SAFETY: caller owns the checked-size output buffer.
        unsafe {
            output.write_unaligned(WordRow {
                abi_version: 1,
                struct_size: size,
                pinyin: Text::from_str(&row.pinyin),
                text: Text::from_str(&row.text),
            });
        }
        Ok(())
    })
}
unsafe fn text<'a>(pointer: *const u8, size: u32) -> Result<&'a str, i32> {
    if pointer.is_null() || size == 0 || size > 128 {
        return Err(INVALID_ARGUMENT);
    }
    // SAFETY: the enclosing FFI caller guarantees this bounded readable input.
    std::str::from_utf8(unsafe { std::slice::from_raw_parts(pointer, size as usize) })
        .map_err(|_| INVALID_ARGUMENT)
}
/// # Safety
/// Both pointers cover the declared readable bytes for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_words_edit(
    handle: u64,
    remove: u32,
    pinyin: *const u8,
    pinyin_size: u32,
    word: *const u8,
    word_size: u32,
) -> i32 {
    boundary(|| {
        if remove > 1 {
            return Err(INVALID_ARGUMENT);
        }
        // SAFETY: caller supplies both readable buffers, validated by text().
        let (pinyin, word) = unsafe { (text(pinyin, pinyin_size)?, text(word, word_size)?) };
        let value = entry(handle)?;
        let mut current = value.model.lock().map_err(|_| INTERNAL_ERROR)?;
        let mut next = (**current).clone();
        if remove == 1 {
            next.remove(pinyin, word)?;
        } else {
            next.add(pinyin, word)?;
        }
        *current = Arc::new(next);
        Ok(())
    })
}
/// # Safety
/// `source` covers `size` readable bytes during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_words_import(handle: u64, source: *const u8, size: u32) -> i32 {
    boundary(|| {
        if source.is_null() || size as usize > crate::user_words::MAX_WORD_FILE_BYTES {
            return Err(INVALID_ARGUMENT);
        }
        // SAFETY: caller owns this bounded readable buffer.
        let next = UserWords::import(unsafe { std::slice::from_raw_parts(source, size as usize) })?;
        *entry(handle)?.model.lock().map_err(|_| INTERNAL_ERROR)? = Arc::new(next);
        Ok(())
    })
}
/// # Safety
/// `written` covers four writable bytes. Nonzero capacity requires that many
/// writable output bytes. Neither buffer requires alignment.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_words_export(
    handle: u64,
    output: *mut u8,
    capacity: u32,
    written: *mut u32,
) -> i32 {
    boundary(|| {
        if written.is_null() || (capacity > 0 && output.is_null()) {
            return Err(INVALID_ARGUMENT);
        }
        let bytes = snapshot(handle)?.export();
        // SAFETY: caller owns the four-byte size output.
        unsafe {
            written.write_unaligned(bytes.len() as u32);
        }
        if capacity == 0 {
            return Ok(());
        }
        if (capacity as usize) < bytes.len() {
            return Err(INVALID_ARGUMENT);
        }
        // SAFETY: caller owns sufficient writable output; bytes is disjoint owned storage.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), output, bytes.len());
        }
        Ok(())
    })
}
