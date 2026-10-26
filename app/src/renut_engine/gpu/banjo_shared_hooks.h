// bejeweled3-nx (tools/renut_hook_remap.py): the addresses both Renut's renderer and Bejeweled 3's own code
// hook. Bejeweled 3's hook calls renut_native_sub_X first when Renut's renderer is chosen
// (BANJO_RENUT_YIELD_SHARED).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

extern "C" void renut_native_sub_820B17B0(PPCContext& ctx, uint8_t* base);  // D3DDevice_Present (feedingfrenzy-nx)
