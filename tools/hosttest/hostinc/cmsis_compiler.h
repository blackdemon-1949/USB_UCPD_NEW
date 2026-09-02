/*
 * Minimal host stand-in for the CMSIS compiler header.
 *
 * usbpd_def.h includes cmsis_compiler.h but only needs a couple of the
 * generic compiler attributes; the ARM-specific intrinsics are never reached
 * by the pure protocol logic under test.  Providing this shim means the host
 * tests compile against the REAL ST header, so the PDO/RDO/EPR bit layouts
 * being tested are the ones the firmware uses rather than a copy.
 */
#ifndef CMSIS_COMPILER_H
#define CMSIS_COMPILER_H

#include <stdint.h>

#define __STATIC_INLINE   static inline
#define __STATIC_FORCEINLINE static inline
#define __INLINE          inline
#define __NO_RETURN
#define __USED
#define __PACKED          __attribute__((packed))
#define __PACKED_STRUCT   struct __attribute__((packed))
#define __ALIGNED(x)      __attribute__((aligned(x)))
#define __WEAK            __attribute__((weak))
#define __ASM             __asm
#define __COMPILER_BARRIER() __asm volatile("" ::: "memory")
#define __NOP()           do {} while (0)

#endif /* CMSIS_COMPILER_H */
