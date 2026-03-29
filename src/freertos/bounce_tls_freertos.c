/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "libbounce/bounce.h"

#define BOUNCE_FREERTOS_INTERNAL_ONLY 1
#include "bounce_freertos_internal.h"

//////////////////////////////////////////////////////////////////////////////////

static BOUNCE_CORE *volatile bounce_tls_fallback_core = NULL;

//////////////////////////////////////////////////////////////////////////////////

void bounce_set_fallback_core(BOUNCE_CORE *b) {
  __atomic_store_n(&bounce_tls_fallback_core, b, __ATOMIC_RELEASE);
}

void bounce_set_core(BOUNCE_CORE *b) {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();

  if (task != NULL) {
    vTaskSetThreadLocalStoragePointer(
      task,
      BOUNCE_FREERTOS_TASK_LOCAL_STORAGE_INDEX,
      b);
  }
}

BOUNCE_CORE *bounce_get_core(void) {
  BOUNCE_CORE *core = NULL;
  TaskHandle_t task = xTaskGetCurrentTaskHandle();

  if (task != NULL) {
    const uintptr_t token =
      (uintptr_t)pvTaskGetThreadLocalStoragePointer(
        task,
        BOUNCE_FREERTOS_TASK_LOCAL_STORAGE_INDEX);

    if ((token & BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG) != 0u) {
      BOUNCE_FREERTOS_DISPATCH_CONTEXT *context =
        (BOUNCE_FREERTOS_DISPATCH_CONTEXT *)
          (token & ~BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG);

      core = (context != NULL) ? context->bounce : NULL;
    } else {
      core = (BOUNCE_CORE *)token;
    }
  }

  return (core != NULL) ?
           core :
           __atomic_load_n(&bounce_tls_fallback_core, __ATOMIC_ACQUIRE);
}
