#ifndef _COMPAT_H_
#define _COMPAT_H_

#include <stddef.h>
#include <strings.h>

/* Mark a declaration as intentionally unused when the compiler supports it. */
#ifndef _U_
#if defined(__has_attribute)
#if __has_attribute(unused)
#define _U_ __attribute__((unused))
#else
#define _U_
#endif
#elif defined(__GNUC__)
#define _U_ __attribute__((unused))
#else
#define _U_
#endif
#endif

/* Secure memory clearing - prefer memset_explicit (C23) over explicit_bzero */
#if !defined(HAVE_MEMSET_EXPLICIT) && !defined(HAVE_EXPLICIT_BZERO)
extern void explicit_bzero(void *s, size_t n);
#elif defined(HAVE_MEMSET_EXPLICIT) && !defined(HAVE_EXPLICIT_BZERO)
#include <string.h>
#define explicit_bzero(s, n) memset_explicit((s), 0, (n))
#endif

#endif
