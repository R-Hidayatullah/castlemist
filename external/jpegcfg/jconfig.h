/* jconfig.h for gw2browser: libjpeg-turbo 3.2.0, MinGW-w64 x64, no SIMD.
 * Hand-written stand-in for the CMake-generated header (see src/jconfig.h.in).
 */
#define JPEG_LIB_VERSION  62
#define LIBJPEG_TURBO_VERSION  "3.2.0"
#define LIBJPEG_TURBO_VERSION_NUMBER  3002000

#define C_ARITH_CODING_SUPPORTED 1
#define D_ARITH_CODING_SUPPORTED 1

/* Support in-memory source/destination managers (jpeg_mem_src) -- required by
 * the browser, which decodes straight out of a dat entry's byte buffer. */
#define MEM_SRCDST_SUPPORTED  1

/* WITH_SIMD deliberately NOT defined (portable C build). */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#ifdef _WIN32

#undef RIGHT_SHIFT_IS_UNSIGNED

#ifndef __RPCNDR_H__
typedef unsigned char boolean;
#endif
#define HAVE_BOOLEAN

#if !(defined(_BASETSD_H_) || defined(_BASETSD_H))
typedef short INT16;
typedef signed int INT32;
#endif
#define XMD_H

#else
#undef RIGHT_SHIFT_IS_UNSIGNED
#endif
