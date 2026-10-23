/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "transinput_core.h"
#define TI_MAX_WORDS 1024u
#define TI_MAX_WORD_FILE_BYTES (320u*1024u)
typedef struct TiWordInfo { uint32_t abi_version,struct_size;uint64_t revision;uint32_t count,reserved; } TiWordInfo;
typedef struct TiWordRow { uint32_t abi_version,struct_size;TiText pinyin,text; } TiWordRow;
#ifdef __cplusplus
extern "C" {
static_assert(sizeof(TiWordInfo)==24);
static_assert(sizeof(TiWordRow)==1040);
#endif
uint64_t ti_words_create(void);
uint64_t ti_words_clone(uint64_t handle);
int32_t ti_words_destroy(uint64_t handle);
int32_t ti_engine_set_words(uint64_t engine,uint64_t handle);
int32_t ti_words_publish(uint64_t destination,uint64_t prepared);
int32_t ti_words_info(uint64_t handle,TiWordInfo* output,uint32_t size);
int32_t ti_words_get(uint64_t handle,uint32_t index,TiWordRow* output,uint32_t size);
int32_t ti_words_edit(uint64_t handle,uint32_t remove,const uint8_t* pinyin,uint32_t pinyin_size,const uint8_t* word,uint32_t word_size);
int32_t ti_words_import(uint64_t handle,const uint8_t* source,uint32_t size);
int32_t ti_words_export(uint64_t handle,uint8_t* output,uint32_t capacity,uint32_t* written);
#ifdef __cplusplus
}
#endif
