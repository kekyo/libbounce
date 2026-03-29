/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

#include "libbounce/bounce.h"

//////////////////////////////////////////////////////////////////////////////////

static INIT_ONCE bounce_tls_init_once_state = INIT_ONCE_STATIC_INIT;
static DWORD bounce_tls_core_index = TLS_OUT_OF_INDEXES;
static PVOID volatile bounce_tls_fallback_core = NULL;

static BOOL CALLBACK bounce_tls_init_once(
  PINIT_ONCE init_once,
  PVOID parameter,
  PVOID *context) {
  (void)init_once;
  (void)parameter;
  (void)context;

  bounce_tls_core_index = TlsAlloc();
  return bounce_tls_core_index != TLS_OUT_OF_INDEXES;
}

static BOOL bounce_tls_ensure_initialized(void) {
  return InitOnceExecuteOnce(
    &bounce_tls_init_once_state,
    bounce_tls_init_once,
    NULL,
    NULL);
}

//////////////////////////////////////////////////////////////////////////////////

void bounce_set_fallback_core(BOUNCE_CORE *b) {
  (void)InterlockedExchangePointer(
    (PVOID volatile *)&bounce_tls_fallback_core,
    b);
}

void bounce_set_core(BOUNCE_CORE *b) {
  if (!bounce_tls_ensure_initialized()) {
    return;
  }

  (void)TlsSetValue(bounce_tls_core_index, b);
}

BOUNCE_CORE *bounce_get_core(void) {
  BOUNCE_CORE *core = NULL;

  if (bounce_tls_ensure_initialized()) {
    core = (BOUNCE_CORE *)TlsGetValue(bounce_tls_core_index);
  }

  return (core != NULL) ?
           core :
           (BOUNCE_CORE *)InterlockedCompareExchangePointer(
             (PVOID volatile *)&bounce_tls_fallback_core,
             NULL,
             NULL);
}
