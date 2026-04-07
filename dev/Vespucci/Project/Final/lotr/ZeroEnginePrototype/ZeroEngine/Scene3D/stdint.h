#pragma once
/*
 * stdint.h polyfill for MSVC 8.0 / Visual Studio 2005.
 * VS2005 does not ship stdint.h (added in VS2010).
 * On _MSC_VER >= 1600 this file is a no-op; the real stdint.h
 * from the VS runtime is used instead.
 */
#if defined(_MSC_VER) && _MSC_VER < 1600

typedef signed   __int8   int8_t;
typedef unsigned __int8   uint8_t;
typedef signed   __int16  int16_t;
typedef unsigned __int16  uint16_t;
typedef signed   __int32  int32_t;
typedef unsigned __int32  uint32_t;
typedef signed   __int64  int64_t;
typedef unsigned __int64  uint64_t;

typedef int8_t    int_least8_t;
typedef uint8_t   uint_least8_t;
typedef int16_t   int_least16_t;
typedef uint16_t  uint_least16_t;
typedef int32_t   int_least32_t;
typedef uint32_t  uint_least32_t;
typedef int64_t   int_least64_t;
typedef uint64_t  uint_least64_t;

typedef int8_t    int_fast8_t;
typedef uint8_t   uint_fast8_t;
typedef int16_t   int_fast16_t;
typedef uint16_t  uint_fast16_t;
typedef int32_t   int_fast32_t;
typedef uint32_t  uint_fast32_t;
typedef int64_t   int_fast64_t;
typedef uint64_t  uint_fast64_t;

typedef int64_t   intmax_t;
typedef uint64_t  uintmax_t;

#if defined(_WIN64)
typedef signed   __int64  intptr_t;
typedef unsigned __int64  uintptr_t;
#else
typedef signed   int      intptr_t;
typedef unsigned int      uintptr_t;
#endif

#endif /* _MSC_VER < 1600 */
