/*
 * sbc_msvc_compat.h
 * Force-included into every google/libsbc translation unit when building with
 * MSVC, so the upstream sources compile unmodified.
 *
 * - Maps GCC/Clang builtins (__builtin_clz, __builtin_expect) onto MSVC equivalents.
 * - Provides a "stdalign.h" shim that evaluates `alignas(...)` to nothing,
 *   since the upstream header uses C11 alignas only to align nested int16_t
 *   arrays on `sizeof(int)` (4-byte) boundaries — natural alignment of int16_t
 *   already satisfies that, so the attribute can be safely dropped.
 */

#ifndef SBC_MSVC_COMPAT_H
#define SBC_MSVC_COMPAT_H

#ifdef _MSC_VER

  /* MSVC's <stdalign.h> requires /std:c11+. Even there it can clash with the
   * way upstream uses `alignas(sizeof(int))` inside a struct field declarator,
   * because MSVC parses it differently. Replace it with an empty token so the
   * fields have natural alignment, which is sufficient for int16_t arrays. */
  #define alignas(x)
  #define _alignas(x)
  #define __alignas_is_defined 1
  #define __alignof_is_defined 1

  /* Make the `#include <stdalign.h>` in upstream headers a no-op. */
  #define _STDALIGN_H
  #define __STDALIGN_H

  #include <intrin.h>

  /* GCC/Clang count-leading-zeros builtin. MSVC has _BitScanReverse. */
  static __inline int __builtin_clz(unsigned int x) {
      unsigned long index;
      if (_BitScanReverse(&index, x)) {
          return 31 - (int)index;
      }
      return 32;
  }

  /* Branch hints — MSVC ignores them, just collapse to the value. */
  #ifndef __builtin_expect
    #define __builtin_expect(expr, val) (expr)
  #endif

#endif /* _MSC_VER */

#endif /* SBC_MSVC_COMPAT_H */
