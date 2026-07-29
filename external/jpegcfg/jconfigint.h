/* jconfigint.h for gw2browser: libjpeg-turbo 3.2.0 internal config (MinGW x64). */
#define BUILD  "gw2browser"

#define HIDDEN  __attribute__((visibility("hidden")))

#undef inline
#define INLINE  inline __attribute__((always_inline))

#define THREAD_LOCAL  __thread

#define PACKAGE_NAME  "libjpeg-turbo"
#define VERSION  "3.2.0"
#define SIZEOF_SIZE_T  8

/* NB: on Win64 sizeof(unsigned long)==4 but sizeof(size_t)==8, so the
 * __builtin_ctzl fast path does NOT apply -- leave HAVE_BUILTIN_CTZL undefined. */

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8
#define C_ARITH_CODING_SUPPORTED 1
#define D_ARITH_CODING_SUPPORTED 1
#endif
