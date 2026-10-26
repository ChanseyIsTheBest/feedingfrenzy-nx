// D3D call trace (PC test aid, 2026-10-26: mapping Renut's hooks to Feeding Frenzy's XDK 2571 Direct3D,
// docs/feedingfrenzy-notes.md): every function of the Direct3D region (0x820B0000-0x820CC000) that code outside it
// calls (96, out/ff_d3d_api.txt) is hooked. With feedingfrenzy_d3d_trace = <game frame>, the log gets, per
// function: its calls up to that frame, whether r3 was the device (the word at 0x82310EE0, which
// Direct3D_CreateDevice fills), its first arguments and caller; and the order of the calls during that frame.
// Off by default; the PC build only.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "feedingfrenzy_capture.h"

REXCVAR_DEFINE_INT32(feedingfrenzy_d3d_trace, 0, "FeedingFrenzy",
                     "PC test aid: log the Direct3D API calls up to and during this game frame (0 = off)");

namespace {

constexpr uint32_t kDeviceGlobal = 0x82310EE0;
struct Stat {
  uint32_t addr = 0;
  uint64_t calls = 0, device_calls = 0;
  uint32_t first[3][6] = {};
};
std::mutex g_mutex;
std::vector<Stat> g_stats;
std::vector<std::string> g_frame_calls;
bool g_dumped = false;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a, 4);
  return __builtin_bswap32(be);
}

void Trace(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  const int32_t at = REXCVAR_GET(feedingfrenzy_d3d_trace);
  if (at <= 0 || g_dumped) return;
  const uint64_t frame = g_feedingfrenzy_game_frames.load(std::memory_order_relaxed);
  const uint32_t device = Load32(base, kDeviceGlobal);
  std::lock_guard<std::mutex> lock(g_mutex);
  Stat* s = nullptr;
  for (auto& x : g_stats) if (x.addr == addr) s = &x;
  if (!s) {
    g_stats.push_back({});
    s = &g_stats.back();
    s->addr = addr;
  }
  const bool is_dev = device && ctx.r3.u32 == device;
  if (s->calls < 3) {
    uint32_t* f = s->first[s->calls];
    f[0] = ctx.r3.u32; f[1] = ctx.r4.u32; f[2] = ctx.r5.u32; f[3] = ctx.r6.u32; f[4] = ctx.r7.u32;
    f[5] = uint32_t(ctx.lr);
  }
  // The shader containers (CreatePixelShader 820B73E8, CreateVertexShader 820B74D0) and the vertex elements
  // (CreateVertexDeclaration 820B6180): their first 96 bytes, on the first three calls.
  if (s->calls < 3 && (addr == 0x820B73E8u || addr == 0x820B74D0u || addr == 0x820B6180u) && ctx.r3.u32) {
    std::string hex;
    for (uint32_t o = 0; o < 96; o += 4) hex += fmt::format(" {:08X}", Load32(base, ctx.r3.u32 + o));
    REXLOG_INFO("[d3dtrace] {:08X} r3 {:08X}:{}", addr, ctx.r3.u32, hex);
  }
  // Every shader container created (virtual part + physical part) to out/ff_shaders/<ps|vs>_<n>.bin, for the
  // container-version study (XDK 2571 writes version 0x0E; Renut and XenosRecomp read 0x11).
  if ((addr == 0x820B73E8u || addr == 0x820B74D0u) && ctx.r3.u32) {
    const uint32_t c = ctx.r3.u32;
    const uint32_t size = Load32(base, c + 4) + Load32(base, c + 8);
    if (size > 0 && size < 0x40000) {
      static int n = 0;
      const std::string path = fmt::format("out/ff_shaders/{}_{:02}.bin", addr == 0x820B73E8u ? "ps" : "vs", n++);
      std::filesystem::create_directories("out/ff_shaders");
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(base + c, 1, size, f);
        std::fclose(f);
      }
    }
  }
  ++s->calls;
  if (is_dev) ++s->device_calls;
  if (frame == uint64_t(at) && g_frame_calls.size() < 4000) {
    g_frame_calls.push_back(fmt::format("{:08X}{} r4={:X} r5={:X} r6={:X} lr={:08X}", addr, is_dev ? " dev" : "",
                                        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr)));
  }
  if (frame > uint64_t(at)) {
    g_dumped = true;
    REXLOG_INFO("[d3dtrace] device 0x{:08X}; {} functions called up to frame {}", device, g_stats.size(), at);
    for (const auto& x : g_stats) {
      std::string first;
      for (uint64_t k = 0; k < x.calls && k < 3; ++k)
        first += fmt::format(" [{:X} {:X} {:X} {:X} {:X} lr {:08X}]", x.first[k][0], x.first[k][1], x.first[k][2],
                             x.first[k][3], x.first[k][4], x.first[k][5]);
      REXLOG_INFO("[d3dtrace] {:08X} calls {} (r3 = device {}):{}", x.addr, x.calls, x.device_calls, first);
    }
    REXLOG_INFO("[d3dtrace] frame {}: {} calls", at, g_frame_calls.size());
    for (const auto& c : g_frame_calls) REXLOG_INFO("[d3dtrace]   {}", c);
  }
}

}  // namespace

#define FF_D3D_TRACE(ADDR)                            extern "C" REX_FUNC(__imp__sub_##ADDR);            REX_HOOK_RAW(sub_##ADDR) {                            Trace(0x##ADDR##u, ctx, base);                     __imp__sub_##ADDR(ctx, base);                    }

FF_D3D_TRACE(820B02F4)
FF_D3D_TRACE(820B0348)
FF_D3D_TRACE(820B0630)
FF_D3D_TRACE(820B0EE8)
FF_D3D_TRACE(820B0F50)
FF_D3D_TRACE(820B17B0)
FF_D3D_TRACE(820B2428)
FF_D3D_TRACE(820B2570)
FF_D3D_TRACE(820B2588)
FF_D3D_TRACE(820B3D50)
FF_D3D_TRACE(820B3D58)
FF_D3D_TRACE(820B3D70)
FF_D3D_TRACE(820B3FB8)
FF_D3D_TRACE(820B4118)
FF_D3D_TRACE(820B57B0)
FF_D3D_TRACE(820B59E8)
FF_D3D_TRACE(820B5B30)
FF_D3D_TRACE(820B5C78)
FF_D3D_TRACE(820B5CD0)
FF_D3D_TRACE(820B5D28)
FF_D3D_TRACE(820B5D78)
FF_D3D_TRACE(820B5DC8)
FF_D3D_TRACE(820B5E18)
FF_D3D_TRACE(820B6158)
FF_D3D_TRACE(820B6160)
FF_D3D_TRACE(820B6180)
FF_D3D_TRACE(820B6308)
FF_D3D_TRACE(820B6310)
FF_D3D_TRACE(820B6410)
FF_D3D_TRACE(820B6420)
FF_D3D_TRACE(820B73E8)
FF_D3D_TRACE(820B74D0)
FF_D3D_TRACE(820B75F8)
FF_D3D_TRACE(820B7600)
FF_D3D_TRACE(820B7608)
FF_D3D_TRACE(820B7630)
FF_D3D_TRACE(820B7638)
FF_D3D_TRACE(820B7640)
FF_D3D_TRACE(820B7C98)
FF_D3D_TRACE(820B7D08)
FF_D3D_TRACE(820B8608)
FF_D3D_TRACE(820B87C8)
FF_D3D_TRACE(820B87D0)
FF_D3D_TRACE(820B9040)
FF_D3D_TRACE(820B9048)
FF_D3D_TRACE(820B9050)
FF_D3D_TRACE(820B9058)
FF_D3D_TRACE(820B90A0)
FF_D3D_TRACE(820B9330)
FF_D3D_TRACE(820B9338)
FF_D3D_TRACE(820B9340)
FF_D3D_TRACE(820B9348)
FF_D3D_TRACE(820B94F8)
FF_D3D_TRACE(820B9500)
FF_D3D_TRACE(820B9508)
FF_D3D_TRACE(820B97B8)
FF_D3D_TRACE(820B9868)
FF_D3D_TRACE(820B9998)
FF_D3D_TRACE(820B9A30)
FF_D3D_TRACE(820B9E98)
FF_D3D_TRACE(820BAA68)
FF_D3D_TRACE(820BB0E0)
FF_D3D_TRACE(820BB270)
FF_D3D_TRACE(820BBD90)
FF_D3D_TRACE(820BBE98)
FF_D3D_TRACE(820BCD90)
FF_D3D_TRACE(820BCE08)
FF_D3D_TRACE(820BCEB8)
FF_D3D_TRACE(820BCF18)
FF_D3D_TRACE(820BCF28)
FF_D3D_TRACE(820BF218)
FF_D3D_TRACE(820CA3B0)
FF_D3D_TRACE(820CA490)
FF_D3D_TRACE(820CAE98)
FF_D3D_TRACE(820CAEA0)
FF_D3D_TRACE(820CAF08)
FF_D3D_TRACE(820CAF60)
FF_D3D_TRACE(820CB208)
FF_D3D_TRACE(820CB268)
FF_D3D_TRACE(820CB2C0)
FF_D3D_TRACE(820CB308)
FF_D3D_TRACE(820CB338)
FF_D3D_TRACE(820CB390)
FF_D3D_TRACE(820CB4F8)
FF_D3D_TRACE(820CB5B8)
FF_D3D_TRACE(820CB670)
FF_D3D_TRACE(820CB860)
FF_D3D_TRACE(820CB8B0)
FF_D3D_TRACE(820CBB50)
FF_D3D_TRACE(820CBC40)
FF_D3D_TRACE(820CBD10)
FF_D3D_TRACE(820CBDA8)
FF_D3D_TRACE(820CBE10)
FF_D3D_TRACE(820CBE80)
FF_D3D_TRACE(820CBEE0)
FF_D3D_TRACE(820CBFC0)
