// sub_822CCD58 - XDK 2571 CRT strstr: r3 = haystack, r4 = needle, returns r3 = the first occurrence or 0.
// feedingfrenzy-nx (2026-10-26): the hottest guest function of the emulator profile (out/renut_tests/emu/prof1: 39 % of
// the main thread's busy samples). 32 instructions:
//   needle[0] == 0         -> `beqlr`: r3 returned as it came (all 64 bits);
//   haystack[0] == 0       -> 0;
//   otherwise for each position p of the haystack (r3 advanced by 1, 64-bit `addi`): compare needle[i] with p[i]
//   (both sign-extended bytes: equality is byte equality) until the needle ends (match: return p) or a byte differs or
//   the haystack ends; then the next position, 0 when the haystack ends.
// That is C's strstr: the first position where the whole needle matches. The native is the host's strstr on the same
// bytes (it reads only within the two strings and their terminators). r3 on a match is haystack + offset in 64 bits,
// as the original's `addi r3,r3,1` steps give.
// Registers: the original leaves r8-r11, cr0 and cr6 as scratch (callers cannot rely on them); the native writes r3 only.
#pragma once
#include <cstring>

#include "../feedingfrenzy_hot_common.h"

namespace feedingfrenzy::hot::n_822CCD58 {

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const char* needle = reinterpret_cast<const char*>(Raw(base, ctx.r4.u32));
  if (!needle[0]) return true;  // r3 unchanged
  const char* haystack = reinterpret_cast<const char*>(Raw(base, ctx.r3.u32));
  const char* found = std::strstr(haystack, needle);
  ctx.r3.u64 = found ? ctx.r3.u64 + uint64_t(found - haystack) : 0;
  return true;
}

inline void Writes(const PPCContext&, const uint8_t*, feedingfrenzy::hot::Writes&) {}  // no stores

}  // namespace feedingfrenzy::hot::n_822CCD58
