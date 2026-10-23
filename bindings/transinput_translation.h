// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {TI_TRANSLATION_SOURCE=800,TI_TRANSLATION_OUTPUT=4096};
typedef struct TiTranslation {
    uint32_t abi_version,struct_size,status,failure;
    uint64_t job,session,revision,configuration,active_job;
    uint32_t cancel_active,source_length,output_length,reserved;
    uint8_t source[TI_TRANSLATION_SOURCE],output[TI_TRANSLATION_OUTPUT];
} TiTranslation;
// Status: 0 disabled, 1 idle, 2 waiting, 3 running, 4 ready, 5 failed.
// Failure: 1 network, 2 timeout, 3 authentication, 4 rate limit, 5 invalid response.
uint64_t ti_translator_create(void);
int32_t ti_translator_destroy(uint64_t handle);
int32_t ti_translator_configure(uint64_t handle,uint64_t configuration,uint32_t enabled);
int32_t ti_translator_observe(uint64_t handle,uint64_t session,uint64_t revision,const uint8_t* input,uint32_t length,uint64_t now_ms);
int32_t ti_translator_clear(uint64_t handle,uint64_t session,uint64_t revision);
// Only the transport actor polls; read-only IPC clients use state(), not poll().
int32_t ti_translator_poll(uint64_t handle,uint64_t now_ms,TiTranslation* output,uint32_t size);
int32_t ti_translator_state(uint64_t handle,TiTranslation* output,uint32_t size);
// Finish only after network buffers/callbacks have been released/acknowledged.
int32_t ti_translator_finish(uint64_t handle,uint64_t job,const uint8_t* input,uint32_t length,uint32_t failure);
int32_t ti_translator_retry(uint64_t handle,uint64_t now_ms);
uint32_t ti_translation_prompt(uint8_t* output,uint32_t capacity);
#ifdef __cplusplus
}
static_assert(sizeof(TiTranslation)==4968);
#endif
