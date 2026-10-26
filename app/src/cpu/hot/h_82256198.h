// h_82256198.h: hand-written native of sub_82256198 (this r3, mask r4): when this->[112] has a bit of `mask` that
// this->[108] lacks, this->[112] = (this->[112] & ~mask) | this->[108], then each child of the vector at this + 296
// (begin +300, end +304, the debug iterator's checks as traps) ORs its [112] into this->[112]; then this->[12]'s
// virtual method 224 / 4 (r4 = mask) when this->[12] is set. Reference: app/generated/hot/n_82256198.h
// (docs/hand-natives.md, docs/optimising-functions.md). The hottest game function of the emulator profile
// (out/renut_tests/emu/prof1: 21 % of the main thread's samples), with the CRT strstr the hottest of all.
//
// Optimisations:
//   - the vector's begin and end words are loaded once: the copy reloads them in every iteration, and the loop's only
//     store (this + 112) cannot reach this + 300 / + 304;
//   - this->[112] is kept in a local between the loop's stores: each iteration still stores it (memory is the copy's
//     after every iteration), and the copy's reload of it reads the value stored the iteration before (no other store
//     in the loop; a child's [112] load comes first and is a load);
//   - the callee sub_82071610 (`lwz r3,112(r3); blr`) runs inline, as in the copy's smart-sync path.
// Audit (the copy's effects in order, and the context it leaves):
//   - ctx.lr read into r12; ctx.lr = 0x822561A0; the back-chain word at r1 - 128 (REX_CHAIN_STORE); r1's low 32 bits
//     - 128;
//   - r11 = [108], r10 = [112], r9 = (r10 & ~r11) & mask; 32-bit test of r9: zero -> the exit with r3 = this,
//     r9 / r10 / r11 those values;
//   - else [112] = (r10 & ~mask) | r11; begin / end; begin > end traps (ppc_trap 22 with the copy's write-back, as the
//     copy); for each p from begin while (end - p) != 0 in 32 bits: p >= end traps, ctx.lr = 0x82256234, the child's
//     [112] ORed into [112] and stored, p >= end traps again, p + 4;
//   - after the loop r9 = begin and r11 = end (the copy's last loads), r10 = 0 (its clz test), r3 = this->[12]; zero:
//     the exit (r3 = 0); else r11 = the vtable word, r4 = mask, ctx.lr = 0x82256278, the copy's write-back, the
//     indirect call, every local reloaded from the context;
//   - the exit: r1 + 128 (64-bit) and the write-back of r1, r3, r9, r10, r11, r12.
#pragma once

namespace feedingfrenzy::hot::h_82256198 {

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  PPCRegister r1 = ctx.r1, r3 = ctx.r3, r4 = ctx.r4, r9 = ctx.r9, r10 = ctx.r10, r11 = ctx.r11, r12;
  const auto trap = [&]() {
    ctx.r1 = r1;
    ctx.r9 = r9;
    ctx.r10 = r10;
    ctx.r11 = r11;
    ctx.r12 = r12;
    ppc_trap(ctx, base, 22);
    r1 = ctx.r1;
    r3 = ctx.r3;
    r4 = ctx.r4;
    r9 = ctx.r9;
    r10 = ctx.r10;
    r11 = ctx.r11;
    r12 = ctx.r12;
  };
  r12.u64 = ctx.lr;
  ctx.lr = 0x822561A0;
  const uint32_t frame = -128 + r1.u32;
  REX_CHAIN_STORE(REX_STORE_U32(frame, r1.u32));
  r1.u32 = frame;
  const uint32_t self = r3.u32;
  const uint64_t mask = r4.u64;
  r11.u64 = REX_LOAD_D_U32(self, 108);
  r10.u64 = REX_LOAD_D_U32(self, 112);
  r9.u64 = (r10.u64 & ~r11.u64) & mask;
  if (r9.s32 != 0) {
    r10.u64 = r10.u64 & ~mask;
    const uint32_t vec = self + 296;
    uint64_t acc = r10.u64 | r11.u64;
    REX_STORE_D_U32(self, 112, uint32_t(acc));
    r10.u64 = REX_LOAD_D_U32(vec, 4);
    r11.u64 = REX_LOAD_D_U32(vec, 8);
    if (r10.u32 > r11.u32) trap();
    const uint32_t begin = r10.u32, end = r11.u32;
    if (begin > end) trap();  // the loop's first check (the copy reloads both words: the same values)
    uint64_t p = begin;
    while (uint32_t(end - uint32_t(p)) != 0) {
      if (!(uint32_t(p) < end)) trap();
      ctx.lr = 0x82256234;
      const uint32_t child = REX_LOAD_D_U32(uint32_t(p), 0);
      acc = uint64_t(REX_LOAD_D_U32(child, 112)) | uint64_t(uint32_t(acc));
      REX_STORE_D_U32(self, 112, uint32_t(acc));
      if (!(uint32_t(p) < end)) trap();
      p = uint64_t(int64_t(p) + 4);
    }
    r9.u64 = begin;
    r11.u64 = end;
    r10.u64 = 0;
    r3.u64 = REX_LOAD_D_U32(self, 12);
    if (r3.u32 != 0) {
      r11.u64 = REX_LOAD_D_U32(r3.u32, 0);
      r4.u64 = mask;
      r11.u64 = REX_LOAD_D_U32(r11.u32, 224);
      ctx.lr = 0x82256278;
      ctx.r1 = r1;
      ctx.r3 = r3;
      ctx.r4 = r4;
      ctx.r9 = r9;
      ctx.r10 = r10;
      ctx.r11 = r11;
      ctx.r12 = r12;
      REX_CALL_INDIRECT_FUNC(r11.u32);
      r1 = ctx.r1;
      r3 = ctx.r3;
      r4 = ctx.r4;
      r9 = ctx.r9;
      r10 = ctx.r10;
      r11 = ctx.r11;
      r12 = ctx.r12;
    }
  }
  r1.s64 = r1.s64 + 128;
  ctx.r1 = r1;
  ctx.r3 = r3;
  ctx.r9 = r9;
  ctx.r10 = r10;
  ctx.r11 = r11;
  ctx.r12 = r12;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }

}  // namespace feedingfrenzy::hot::h_82256198
