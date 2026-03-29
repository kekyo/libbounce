/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_GENERIC)

#include <stddef.h>

#include "libbounce/bounce.h"

//////////////////////////////////////////////////////////////////////////////////

static _Thread_local BOUNCE_CORE *bounce_tls_core = NULL;
static BOUNCE_CORE *bounce_tls_fallback_core = NULL;

void bounce_set_core(BOUNCE_CORE *b) {
  bounce_tls_core = b;
}

void bounce_set_fallback_core(BOUNCE_CORE *b) {
  __atomic_store_n(&bounce_tls_fallback_core, b, __ATOMIC_RELEASE);
}

BOUNCE_CORE *bounce_get_core(void) {
  return (bounce_tls_core != NULL) ?
           bounce_tls_core :
           __atomic_load_n(&bounce_tls_fallback_core, __ATOMIC_ACQUIRE);
}

#endif
