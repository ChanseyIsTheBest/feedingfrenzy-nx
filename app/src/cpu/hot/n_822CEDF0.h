// sub_822CEDF0 - XDK 2571 CRT memmove: r3 = dst, r4 = src, r5 = n, returns r3 = dst.
// feedingfrenzy-nx (2026-10-26): ninth in the emulator profile (out/renut_tests/emu/prof1). 48 instructions:
//   dst == src           -> `beqlr`;
//   dst < src (signed)   -> tail call of memcpy sub_822CD480 (forward: memmove for dst < src, a plain copy otherwise);
//   dst > src            -> backward from dst + n / src + n: bytes down to word alignment of dst, then words (lwz/stw
//                           when src is aligned, four lbz assembled otherwise), then the last bytes; r3 ends at dst.
// Both paths give memmove's result (the signed compare only picks the direction; ranges far apart do not overlap), and
// r3 is dst. n == 0 copies nothing on either path. Sizes and addresses are 32-bit sums in the backward path except
// `add r3,r3,r5` / `add r4,r4,r5` (64-bit): callers pass zero-extended values. Ranges that wrap around 4 GB are declined
// (the original runs).
// Registers: the original leaves r0, r4-r10, ctr and cr0 as scratch; the native writes none (r3 stays dst).
#pragma once
#include <cstring>

#include "../feedingfrenzy_hot_common.h"

namespace feedingfrenzy::hot::n_822CEDF0 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0 || dst == src) return true;
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  // The direction is chosen by a SIGNED compare: for overlapping ranges on both sides of 0x80000000 the original would
  // copy backward with dst < src (or forward with dst > src) and smear; declined (the original runs).
  if (((dst ^ src) & 0x80000000u) && (uint32_t(dst - src) < n || uint32_t(src - dst) < n)) return false;
  std::memmove(Raw(base, dst), Raw(base, src), n);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, feedingfrenzy::hot::Writes& w) {
  w.Add(ctx.r3.u32, ctx.r5.u32);  // > 1 MB sets overflow
}

}  // namespace feedingfrenzy::hot::n_822CEDF0
