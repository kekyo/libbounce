/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_POSIX)

#include <pthread.h>

#include "libbounce/bounce.h"

//////////////////////////////////////////////////////////////////////////////////

static pthread_once_t bounce_tls_init_once_state = PTHREAD_ONCE_INIT;
static pthread_key_t bounce_tls_core_key;
static BOUNCE_CORE *volatile bounce_tls_fallback_core = NULL;

static void bounce_tls_init_once(void) {
  (void)pthread_key_create(&bounce_tls_core_key, NULL);
}

static bool bounce_tls_ensure_initialized(void) {
  return pthread_once(
           &bounce_tls_init_once_state,
           bounce_tls_init_once) == 0;
}

//////////////////////////////////////////////////////////////////////////////////

void bounce_set_fallback_core(BOUNCE_CORE *b) {
  __atomic_store_n(&bounce_tls_fallback_core, b, __ATOMIC_RELEASE);
}

void bounce_set_core(BOUNCE_CORE *b) {
  if (!bounce_tls_ensure_initialized()) {
    return;
  }

  (void)pthread_setspecific(bounce_tls_core_key, b);
}

BOUNCE_CORE *bounce_get_core(void) {
  BOUNCE_CORE *core = NULL;

  if (bounce_tls_ensure_initialized()) {
    core = (BOUNCE_CORE *)pthread_getspecific(bounce_tls_core_key);
  }

  return (core != NULL) ?
           core :
           __atomic_load_n(&bounce_tls_fallback_core, __ATOMIC_ACQUIRE);
}

#endif
