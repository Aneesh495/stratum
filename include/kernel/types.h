#ifndef STRATUM_KERNEL_TYPES_H
#define STRATUM_KERNEL_TYPES_H

#ifdef __INT64_TYPE__
typedef __INT8_TYPE__      int8_t;
typedef __INT16_TYPE__     int16_t;
typedef __INT32_TYPE__     int32_t;
typedef __INT64_TYPE__     int64_t;

typedef __UINT8_TYPE__     uint8_t;
typedef __UINT16_TYPE__    uint16_t;
typedef __UINT32_TYPE__    uint32_t;
typedef __UINT64_TYPE__    uint64_t;

typedef __INTPTR_TYPE__    intptr_t;
typedef __UINTPTR_TYPE__   uintptr_t;
typedef __SIZE_TYPE__      size_t;
#else
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef int64_t            intptr_t;
typedef uint64_t           uintptr_t;
typedef uint64_t           size_t;
#endif

#if !__STDC_HOSTED__
typedef int64_t            ssize_t;
typedef int64_t            off_t;
typedef int32_t            pid_t;
#endif
typedef int32_t            tid_t;

#ifndef bool
typedef _Bool              bool;
#define true               1
#define false              0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define container_of(ptr, type, member) ({                      \
    const typeof(((type *)0)->member) *__mptr = (ptr);          \
    (type *)((char *)__mptr - offsetof(type, member));          \
})

#define min(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a > _b ? _a : _b; })
#define round_up(n, d) (((n) + (d) - 1) & ~((d) - 1))
#define round_down(n, d) ((n) & ~((d) - 1))

#endif /* STRATUM_KERNEL_TYPES_H */
