// fix_stdint.h — Because Microsoft Can't Keep Their Own Shit Stable
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// MSVC 14.44 introduced a REGRESSION where <cstdint> can't find standard
// integer types. A REGRESSION. In 2024. In a COMPILER. They broke stdint.
// The most basic header in C. So we manually typedef every integer type
// like it's 1998 and force-include this before everything else.
//
// The stolen Havok 5.5 SDK was compiled with VS2005. The ImGui DLL
// uses VS2022. The main engine bounces between both. Every compiler
// version has its own opinion about where uint32_t lives. This file
// says "shut the fuck up, here are your types, stop arguing."
// -----------------------------------------------------------------------
#ifndef FIX_STDINT_H
#define FIX_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

typedef int8_t   int_fast8_t;
typedef int32_t  int_fast16_t;
typedef int32_t  int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint8_t  uint_fast8_t;
typedef uint32_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;

typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

#ifdef _WIN64
typedef int64_t  intptr_t;
typedef uint64_t uintptr_t;
#else
typedef int32_t  intptr_t;
typedef uint32_t uintptr_t;
#endif

#endif
