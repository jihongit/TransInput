// SPDX-License-Identifier: AGPL-3.0-only
//! C ABI v1: opaque monotonic handles and caller-owned bounded snapshots.
//! Caller pointers must reference writable storage for the declared size.
//! No references to caller storage are formed; unaligned C buffers are supported.
use crate::{Engine, Event, Outcome};
use std::{
    collections::HashMap,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU64, Ordering},
    },
};

pub const ABI_VERSION: u32 = 1;
pub const TEXT_CAPACITY: usize = 512;
const MAX_HANDLES: usize = 256;
pub(crate) const INVALID_ARGUMENT: i32 = -1;
pub(crate) const INVALID_HANDLE: i32 = -2;
pub(crate) const INTERNAL_ERROR: i32 = -3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Text {
    pub length: u32,
    pub bytes: [u8; TEXT_CAPACITY],
}
impl Text {
    fn empty() -> Self {
        Self {
            length: 0,
            bytes: [0; TEXT_CAPACITY],
        }
    }
    pub(crate) fn from_str(source: &str) -> Self {
        let mut result = Self::empty();
        // Engine text is bounded to this exact capacity; never split UTF-8.
        let mut length = source.len().min(TEXT_CAPACITY);
        while !source.is_char_boundary(length) {
            length -= 1;
        }
        result.bytes[..length].copy_from_slice(&source.as_bytes()[..length]);
        result.length = length as u32;
        result
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    pub abi_version: u32,
    pub struct_size: u32,
    pub revision: u64,
    pub composing: u32,
    pub cursor: u32,
    pub selected: u32,
    pub candidate_count: u32,
    pub preedit: Text,
    pub candidates: [Text; 9],
    pub commit: Text,
    /// 0 pass-through, 1 update, 2 commit, 3 cancel, 4 input-limit.
    pub outcome: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CandidatePage {
    pub abi_version: u32,
    pub struct_size: u32,
    pub page: u32,
    pub total: u32,
    pub snapshot: Snapshot,
}

type SharedEngine = Arc<Mutex<Engine>>;
static ENGINES: OnceLock<Mutex<HashMap<u64, SharedEngine>>> = OnceLock::new();
static DICTIONARY: OnceLock<Mutex<Option<Arc<crate::Dictionary>>>> = OnceLock::new();
static NEXT_HANDLE: AtomicU64 = AtomicU64::new(1);

fn registry() -> &'static Mutex<HashMap<u64, SharedEngine>> {
    ENGINES.get_or_init(Mutex::default)
}
pub(crate) fn engine(handle: u64) -> Result<SharedEngine, i32> {
    registry()
        .lock()
        .map_err(|_| INTERNAL_ERROR)?
        .get(&handle)
        .cloned()
        .ok_or(INVALID_HANDLE)
}
pub(crate) fn dictionary() -> Result<Arc<crate::Dictionary>, i32> {
    let mut cached = DICTIONARY
        .get_or_init(|| Mutex::new(None))
        .lock()
        .map_err(|_| INTERNAL_ERROR)?;
    Ok(cached
        .get_or_insert_with(|| Arc::new(crate::bundled_dictionary()))
        .clone())
}
pub(crate) fn boundary(action: impl FnOnce() -> Result<(), i32>) -> i32 {
    catch_unwind(AssertUnwindSafe(action))
        .unwrap_or(Err(INTERNAL_ERROR))
        .map_or_else(|code| code, |()| 0)
}

#[unsafe(no_mangle)]
pub extern "C" fn ti_abi_version() -> u32 {
    ABI_VERSION
}

#[unsafe(no_mangle)]
pub extern "C" fn ti_snapshot_size() -> u32 {
    std::mem::size_of::<Snapshot>() as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn ti_engine_create() -> u64 {
    catch_unwind(|| {
        let mut engines = registry().lock().ok()?;
        if engines.len() == MAX_HANDLES {
            return None;
        }
        // Parse the real base dictionary once per library lifetime. The explicit
        // shutdown below releases this cache; closing a context must not force a
        // full reload when the next context starts.
        let dictionary = dictionary().ok()?;
        // All allocation is serialized by the registry lock; no handle reuse.
        let handle = NEXT_HANDLE.load(Ordering::Relaxed);
        NEXT_HANDLE.store(handle.checked_add(1)?, Ordering::Relaxed);
        engines.insert(handle, Arc::new(Mutex::new(Engine::new(dictionary))));
        Some(handle)
    })
    .ok()
    .flatten()
    .unwrap_or(0)
}

/// Call after all handles and concurrent calls are finished, before unloading.
/// -4 means an engine/call still owns library state; unloading is not yet safe.
#[unsafe(no_mangle)]
pub extern "C" fn ti_shutdown() -> i32 {
    boundary(|| {
        crate::translation_ffi::shutdown_check()?;
        crate::learning_ffi::shutdown_check()?;
        crate::user_words_ffi::shutdown_check()?;
        let mut engines = registry().lock().map_err(|_| INTERNAL_ERROR)?;
        if !engines.is_empty() {
            return Err(-4);
        }
        if let Some(cache) = DICTIONARY.get() {
            let mut dictionary = cache.lock().map_err(|_| INTERNAL_ERROR)?;
            if dictionary
                .as_ref()
                .is_some_and(|value| Arc::strong_count(value) != 1)
            {
                return Err(-4);
            }
            *dictionary = None;
        }
        engines.shrink_to_fit();
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ti_engine_destroy(handle: u64) -> i32 {
    boundary(|| {
        registry()
            .lock()
            .map_err(|_| INTERNAL_ERROR)?
            .remove(&handle)
            .ok_or(INVALID_HANDLE)?;
        Ok(())
    })
}

/// Query a full composition without generating a commit or replaying key events.
/// # Safety
/// `input` must cover `length` readable bytes (or be null when length is zero).
/// `output` must cover `output_size` writable bytes for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_engine_query(
    handle: u64,
    input: *const u8,
    length: u32,
    cursor: u32,
    output: *mut Snapshot,
    output_size: u32,
) -> i32 {
    boundary(|| {
        validate_output(output, output_size)?;
        if length > crate::dictionary::MAX_INPUT_BYTES as u32 || (length != 0 && input.is_null()) {
            return Err(INVALID_ARGUMENT);
        }
        let bytes = if length == 0 {
            &[][..]
        } else {
            // SAFETY: caller guarantees length readable bytes; length and null were checked.
            unsafe { std::slice::from_raw_parts(input, length as usize) }
        };
        let source = std::str::from_utf8(bytes).map_err(|_| INVALID_ARGUMENT)?;
        let shared = engine(handle)?;
        let mut engine = shared.lock().map_err(|_| INTERNAL_ERROR)?;
        if !engine.replace_composition(source, cursor as usize) {
            return Err(INVALID_ARGUMENT);
        }
        let snapshot = make_snapshot(&engine, Outcome::Updated);
        // SAFETY: caller guarantees writable storage; null and capacity were checked.
        unsafe { output.write_unaligned(snapshot) };
        Ok(())
    })
}

fn make_snapshot(engine: &Engine, outcome: Outcome) -> Snapshot {
    let mut snapshot = Snapshot {
        abi_version: ABI_VERSION,
        struct_size: ti_snapshot_size(),
        revision: engine.revision(),
        composing: u32::from(engine.composing()),
        cursor: engine.cursor() as u32,
        selected: engine.selected() as u32,
        candidate_count: engine.candidates().len() as u32,
        preedit: Text::from_str(engine.preedit()),
        candidates: [Text::empty(); 9],
        commit: Text::empty(),
        outcome: 0,
    };
    for (index, text) in engine.candidates().iter().enumerate() {
        snapshot.candidates[index] = Text::from_str(text);
    }
    snapshot.outcome = match outcome {
        Outcome::PassThrough => 0,
        Outcome::Updated => 1,
        Outcome::Commit(text) => {
            snapshot.commit = Text::from_str(&text);
            2
        }
        Outcome::Cancelled => 3,
        Outcome::InputLimit => 4,
    };
    snapshot
}

/// # Safety
/// `output` must point to storage of `output_size` bytes writable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_engine_apply(
    handle: u64,
    key: u32,
    argument: u32,
    output: *mut Snapshot,
    output_size: u32,
) -> i32 {
    boundary(|| {
        validate_output(output, output_size)?;
        let shared = engine(handle)?;
        let mut engine = shared.lock().map_err(|_| INTERNAL_ERROR)?;
        let outcome = if key == 12 {
            engine.reset();
            Outcome::Cancelled
        } else {
            let event = match key {
                1 => Event::Letter(char::from_u32(argument).ok_or(INVALID_ARGUMENT)?),
                2 => Event::Backspace,
                3 => Event::Delete,
                4 => Event::Left,
                5 => Event::Right,
                6 => Event::Up,
                7 => Event::Down,
                8 => Event::Space,
                9 => Event::Enter,
                10 => Event::Escape,
                11 => Event::Select(argument as usize),
                13 => Event::PageUp,
                14 => Event::PageDown,
                _ => return Err(INVALID_ARGUMENT),
            };
            engine.apply(event)
        };
        let snapshot = make_snapshot(&engine, outcome);
        // SAFETY: caller guarantees writable storage; null/size were checked.
        // MSVC x86 may give otherwise valid stack buffers only 4-byte alignment.
        unsafe { output.write_unaligned(snapshot) };
        Ok(())
    })
}

/// # Safety
/// `output` must point to storage of `output_size` bytes writable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_engine_snapshot(
    handle: u64,
    output: *mut Snapshot,
    output_size: u32,
) -> i32 {
    boundary(|| {
        validate_output(output, output_size)?;
        let shared = engine(handle)?;
        let engine = shared.lock().map_err(|_| INTERNAL_ERROR)?;
        let snapshot = make_snapshot(&engine, Outcome::Updated);
        // SAFETY: caller guarantees writable storage; null/size were checked.
        unsafe { output.write_unaligned(snapshot) };
        Ok(())
    })
}

/// Read one clamped page without replaying input or committing text.
/// # Safety
/// `output` covers `output_size` writable bytes; alignment is not required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ti_engine_page(
    handle: u64,
    page: u32,
    output: *mut CandidatePage,
    output_size: u32,
) -> i32 {
    boundary(|| {
        if output.is_null()
            || (output_size as usize) < std::mem::size_of::<CandidatePage>()
            || page >= 10
        {
            return Err(INVALID_ARGUMENT);
        }
        let shared = engine(handle)?;
        let mut engine = shared.lock().map_err(|_| INTERNAL_ERROR)?;
        engine.set_page(page as usize);
        let value = CandidatePage {
            abi_version: ABI_VERSION,
            struct_size: std::mem::size_of::<CandidatePage>() as u32,
            page: engine.page() as u32,
            total: engine.candidate_total() as u32,
            snapshot: make_snapshot(&engine, Outcome::Updated),
        };
        // SAFETY: caller owns output_size writable bytes; null and capacity checked.
        unsafe { output.write_unaligned(value) };
        Ok(())
    })
}

fn validate_output(output: *mut Snapshot, size: u32) -> Result<(), i32> {
    if output.is_null() || size < ti_snapshot_size() {
        Err(INVALID_ARGUMENT)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::MaybeUninit;
    #[test]
    fn candidate_page_is_additive_bounded_and_supports_unaligned_output() {
        assert_eq!(std::mem::size_of::<Snapshot>(), 5712);
        assert_eq!(std::mem::size_of::<CandidatePage>(), 5728);
        let handle = ti_engine_create();
        let mut snapshot = MaybeUninit::<Snapshot>::uninit();
        assert_eq!(
            // SAFETY: fixed source and caller-owned complete snapshot buffer.
            unsafe {
                ti_engine_query(
                    handle,
                    b"n".as_ptr(),
                    1,
                    1,
                    snapshot.as_mut_ptr(),
                    ti_snapshot_size(),
                )
            },
            0
        );
        let size = std::mem::size_of::<CandidatePage>();
        let mut bytes = vec![0xa5; size + 2];
        let output = bytes.as_mut_ptr().wrapping_add(1).cast::<CandidatePage>();
        // SAFETY: size writable bytes from offset one; deliberate unaligned ABI use.
        assert_eq!(unsafe { ti_engine_page(handle, 1, output, size as u32) }, 0);
        // SAFETY: the successful call initialized the unaligned CandidatePage.
        let page = unsafe { output.read_unaligned() };
        assert_eq!(page.page, 1);
        assert_eq!(page.total, 90);
        assert_eq!(page.snapshot.candidate_count, 9);
        assert_eq!(bytes[0], 0xa5);
        assert_eq!(bytes[size + 1], 0xa5);
        let before = bytes.clone();
        assert_eq!(
            // SAFETY: invalid page must be rejected without writes or state changes.
            unsafe { ti_engine_page(handle, 10, output, size as u32) },
            INVALID_ARGUMENT
        );
        assert_eq!(
            // SAFETY: same buffer, insufficient declared capacity is intentionally rejected.
            unsafe { ti_engine_page(handle, 0, output, 1) },
            INVALID_ARGUMENT
        );
        assert_eq!(bytes, before);
        assert_eq!(
            // SAFETY: valid caller-owned writable snapshot buffer.
            unsafe { ti_engine_snapshot(handle, snapshot.as_mut_ptr(), ti_snapshot_size()) },
            0
        );
        // SAFETY: the preceding successful call initialized snapshot.
        let retained = unsafe { snapshot.assume_init() };
        assert_eq!(
            retained.candidates[0].bytes,
            page.snapshot.candidates[0].bytes
        );
        assert_eq!(ti_engine_destroy(handle), 0);
    }
    #[test]
    fn unaligned_c_buffer_is_written_without_forming_a_reference() {
        let handle = ti_engine_create();
        let size = ti_snapshot_size();
        let mut bytes = vec![0xa5; size as usize + 8];
        let offset = (0..8)
            .find(|offset| (bytes.as_ptr() as usize + offset) % 8 == 1)
            .unwrap();
        let output = bytes[offset..].as_mut_ptr().cast::<Snapshot>();
        assert_eq!(
            // SAFETY: buffer has size bytes; the ABI explicitly permits unaligned storage.
            unsafe { ti_engine_apply(handle, 1, 'n' as u32, output, size) },
            0
        );
        // SAFETY: successful call initialized size bytes; never dereference the unaligned pointer.
        let snapshot = unsafe { output.read_unaligned() };
        assert_eq!(snapshot.preedit.length, 1);
        assert_eq!(snapshot.preedit.bytes[0], b'n');
        assert_eq!(snapshot.abi_version, ABI_VERSION);
        assert_eq!(bytes[offset + size as usize], 0xa5);
        assert_eq!(ti_engine_destroy(handle), 0);
    }
    #[test]
    fn owned_snapshot_survives_destroy_and_commit_is_not_replayed() {
        let handle = ti_engine_create();
        assert_ne!(handle, 0);
        let mut output = MaybeUninit::<Snapshot>::uninit();
        for c in "nihao".chars() {
            assert_eq!(
                // SAFETY: output owns correctly sized aligned writable Snapshot storage.
                unsafe {
                    ti_engine_apply(handle, 1, c as u32, output.as_mut_ptr(), ti_snapshot_size())
                },
                0
            );
        }
        // SAFETY: the preceding successful call initialized output.
        let before = unsafe { output.assume_init() };
        assert_eq!(&before.candidates[0].bytes[..6], "你好".as_bytes());
        assert_eq!(
            // SAFETY: output still owns writable Snapshot storage.
            unsafe { ti_engine_apply(handle, 8, 0, output.as_mut_ptr(), ti_snapshot_size()) },
            0
        );
        // SAFETY: the successful call initialized output.
        assert_eq!(unsafe { output.assume_init() }.commit.length, 6);
        assert_eq!(
            // SAFETY: output still owns writable Snapshot storage.
            unsafe { ti_engine_snapshot(handle, output.as_mut_ptr(), ti_snapshot_size()) },
            0
        );
        // SAFETY: the successful snapshot call initialized output.
        assert_eq!(unsafe { output.assume_init() }.commit.length, 0);
        assert_eq!(ti_engine_destroy(handle), 0);
        assert_eq!(ti_engine_destroy(handle), INVALID_HANDLE);
        assert_eq!(&before.candidates[0].bytes[..6], "你好".as_bytes());
    }
    #[test]
    fn invalid_buffer_does_not_consume_input() {
        let handle = ti_engine_create();
        assert_eq!(
            // SAFETY: null is deliberately rejected before dereference.
            unsafe { ti_engine_apply(handle, 1, 'n' as u32, std::ptr::null_mut(), 0) },
            INVALID_ARGUMENT
        );
        let mut output = MaybeUninit::<Snapshot>::uninit();
        assert_eq!(
            // SAFETY: output is valid and aligned; undersized storage is rejected before write.
            unsafe { ti_engine_apply(handle, 1, 'n' as u32, output.as_mut_ptr(), 1) },
            INVALID_ARGUMENT
        );
        assert_eq!(
            // SAFETY: correct writable storage and declared size.
            unsafe { ti_engine_snapshot(handle, output.as_mut_ptr(), ti_snapshot_size()) },
            0
        );
        // SAFETY: successful call initialized output.
        assert_eq!(unsafe { output.assume_init() }.preedit.length, 0);
        assert_eq!(ti_engine_destroy(handle), 0);
    }
    #[test]
    fn stale_handles_and_unknown_keys_are_rejected() {
        let mut output = MaybeUninit::<Snapshot>::uninit();
        assert_eq!(
            // SAFETY: output is valid writable storage; invalid handle is checked before use.
            unsafe { ti_engine_snapshot(0, output.as_mut_ptr(), ti_snapshot_size()) },
            INVALID_HANDLE
        );
        let handle = ti_engine_create();
        assert_eq!(
            // SAFETY: output owns valid storage; unknown key is rejected.
            unsafe {
                ti_engine_apply(handle, u32::MAX, 0, output.as_mut_ptr(), ti_snapshot_size())
            },
            INVALID_ARGUMENT
        );
        assert_eq!(ti_engine_destroy(handle), 0);
    }
}
