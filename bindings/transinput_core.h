// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TI_ABI_VERSION = 1, TI_TEXT_CAPACITY = 512, TI_CANDIDATES = 9, TI_MAX_PAGES = 10 };
enum TiKey {
    TI_LETTER = 1, TI_BACKSPACE = 2, TI_DELETE = 3, TI_LEFT = 4,
    TI_RIGHT = 5, TI_UP = 6, TI_DOWN = 7, TI_SPACE = 8, TI_ENTER = 9,
    TI_ESCAPE = 10, TI_SELECT = 11, TI_RESET = 12, TI_PAGE_UP = 13, TI_PAGE_DOWN = 14
};

typedef struct TiText { uint32_t length; uint8_t bytes[TI_TEXT_CAPACITY]; } TiText;
typedef struct TiSnapshot {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t revision;
    uint32_t composing;
    uint32_t cursor;
    uint32_t selected;
    uint32_t candidate_count;
    TiText preedit;
    TiText candidates[TI_CANDIDATES];
    TiText commit;
    uint32_t outcome;
} TiSnapshot;

typedef struct TiCandidatePage {
    uint32_t abi_version, struct_size, page, total;
    TiSnapshot snapshot;
} TiCandidatePage;

// Snapshots are caller-owned and at least ti_snapshot_size() writable bytes.
// The ABI writes safely even if a native stack buffer is only 4-byte aligned.
// Strings are length-delimited UTF-8, not NUL-terminated. Handles are not pointers.
uint32_t ti_abi_version(void);
uint32_t ti_snapshot_size(void);
uint64_t ti_engine_create(void);
int32_t ti_engine_destroy(uint64_t handle);
// All handles/calls must be finished before shutdown and DLL unload (-4 = busy).
int32_t ti_shutdown(void);
int32_t ti_engine_apply(uint64_t handle, uint32_t key, uint32_t argument,
    TiSnapshot* output, uint32_t output_size);
int32_t ti_engine_snapshot(uint64_t handle, TiSnapshot* output, uint32_t output_size);
// Whole-composition query; no commit side effect. At most 128 ASCII input bytes.
int32_t ti_engine_query(uint64_t handle, const uint8_t* input, uint32_t length,
    uint32_t cursor, TiSnapshot* output, uint32_t output_size);
// Additive v1 API; page [0,9] clamps to the last available page. At most 90
// total candidates, 9 per snapshot; TI_SELECT remains local to the current page.
int32_t ti_engine_page(uint64_t handle, uint32_t page, TiCandidatePage* output, uint32_t output_size);

#ifdef __cplusplus
}
#endif
