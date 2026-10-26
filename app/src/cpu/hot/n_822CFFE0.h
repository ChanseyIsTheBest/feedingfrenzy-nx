// sub_822CFFE0 - XDK 2571 CRT _stricmp: r3 = a, r4 = b, returns r3 = the difference at the first mismatch.
// feedingfrenzy-nx (2026-10-26): sixth in the emulator profile (out/renut_tests/emu/prof1). 22 instructions:
//   for i = 0, 1, ...: ca = a[i], cb = b[i] (lbzu: unsigned bytes);
//     r3 = ca - cb; if cb == 0 return r3 (= ca: the byte of a as it is, NOT lower-cased);
//     if ca == cb continue;
//     cb |= 0x20 if 'A' <= cb <= 'Z'; ca |= 0x20 if 'A' <= ca <= 'Z'; r3 = ca - cb; if r3 == 0 continue; return r3.
// r3 is the 64-bit difference of the two zero-extended bytes (negative values sign-extended). A host _stricmp /
// strcasecmp would lower-case before the end test (`"A"` against `""` gives 65 here, 97 there): the loop is written out.
// Registers: the original leaves r4-r6, r9 and cr0/cr1/cr5/cr6/cr7 as scratch (r4 advanced by lbzu); the native writes
// r3 only.
#pragma once
#include "../feedingfrenzy_hot_common.h"

namespace feedingfrenzy::hot::n_822CFFE0 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint8_t* a = Raw(base, ctx.r3.u32);
  const uint8_t* b = Raw(base, ctx.r4.u32);
  for (uint32_t i = 0;; ++i) {
    int64_t ca = a[i], cb = b[i];
    if (cb == 0) {
      ctx.r3.s64 = ca;
      return true;
    }
    if (ca == cb) continue;
    if (cb >= 'A' && cb <= 'Z') cb |= 0x20;
    if (ca >= 'A' && ca <= 'Z') ca |= 0x20;
    if (ca != cb) {
      ctx.r3.s64 = ca - cb;
      return true;
    }
  }
}

inline void Writes(const PPCContext&, const uint8_t*, feedingfrenzy::hot::Writes&) {}  // no stores

}  // namespace feedingfrenzy::hot::n_822CFFE0
