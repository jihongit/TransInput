// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "transinput_core.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { TI_MAX_LEARNING_BYTES = 512 * 1024 };
uint64_t ti_learning_create(void);
int32_t ti_learning_destroy(uint64_t handle);
// Caller-owned four-byte result; unaligned storage is supported.
int32_t ti_learning_count(uint64_t handle,uint32_t* count);
// Zero detaches. Existing engine candidates change on the next query/edit.
int32_t ti_engine_set_learning(uint64_t engine,uint64_t learning);
// Call only after successful explicit Chinese selection, outside sensitive input.
// Arbitrary text absent from the dictionary's candidate set is rejected.
int32_t ti_learning_record(uint64_t handle,const uint8_t* input,uint32_t input_size,const uint8_t* text,uint32_t text_size);
// Additional immutable personal profile used for candidate validation; zero is base-only.
int32_t ti_learning_record_with_words(uint64_t handle,uint64_t words,const uint8_t* input,uint32_t input_size,const uint8_t* text,uint32_t text_size);
int32_t ti_learning_import(uint64_t handle,const uint8_t* data,uint32_t size);
// Output is caller-owned, written permits unaligned storage. Null+0 queries size.
// Use TI_MAX_LEARNING_BYTES to avoid a concurrent size-query race.
int32_t ti_learning_export(uint64_t handle,uint8_t* data,uint32_t capacity,uint32_t* written);
#ifdef __cplusplus
}
#endif
