/*
 * Secure memory clearing implementation for systems without explicit_bzero
 *
 * Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#if !defined(HAVE_EXPLICIT_BZERO) && !defined(HAVE_MEMSET_EXPLICIT)

/*
 * Secure memory clearing function that won't be optimized by the compiler.
 *
 * This uses a volatile function pointer technique which prevents the compiler
 * from optimizing away the memset call.
 */
void explicit_bzero(void *s, size_t n)
{
    /* Use volatile function pointer to prevent dead store elimination */
    static void *(*const volatile memset_ptr)(void *, int, size_t) = memset;
    (memset_ptr)(s, 0, n);
}

#endif /* !HAVE_EXPLICIT_BZERO && !HAVE_MEMSET_EXPLICIT */
