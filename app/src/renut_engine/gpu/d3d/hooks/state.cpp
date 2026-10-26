/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x32C8 + i * 4)));
  viewport[4] = d.W(0x32D8);
  viewport[5] = d.W(0x32DC);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3604));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_820B9E98, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9F20, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9EE8, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B97F0, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B97B8, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9868, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9C30, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9D78, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9998, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9A30, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9900, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9B40, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9BB8, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9AC8, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9828, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9CD0, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9D40, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9F60, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9FB0, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA028, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA068, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA0A8, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9FF0, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA1D0, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA1F8, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA220, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA118, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA158, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA198, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA0E0, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA248, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA270, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA298, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // N&B sub_8228F370
BANJO_RENUT_HOOK(sub_820BA5D0, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA628, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA5C0, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA678, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA460, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA498, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // N&B sub_8264C4A8
BANJO_RENUT_HOOK(sub_820BA300, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA3C0, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA4C0, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA500, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA540, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA580, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BADE0, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAD80, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BADB0, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA6C8, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA6F0, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA720, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA750, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA780, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA7B0, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA7E0, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA810, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA838, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA860, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA890, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA8C0, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA8F0, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA920, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA950, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA980, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAA68, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // N&B sub_8264CAF0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // N&B sub_8264CB88
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // N&B sub_8264CC20
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // N&B sub_8264CCB8
BANJO_RENUT_HOOK(sub_820BAE10, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAE48, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAE80, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAEA8, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAEE0, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAF20, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAF50, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAF80, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAFB0, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAFE0, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB018, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB050, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB088, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0B0, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0C0, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable  // N&B sub_8264CFA8
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable  // N&B sub_822899D0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel  // N&B sub_8264C060
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth  // N&B sub_8264C098
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames  // N&B sub_8264C110

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_820BB7F8, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB848, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB898, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB790, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB270, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0E0, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB3F8, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB600, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB6A0, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB4E0, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB368, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB1D8, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB448, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB718, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB8E8, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB558, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB938, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB988, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB9D8, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BBA28, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // feedingfrenzy-nx: state block alignment

BANJO_RENUT_HOOK(sub_820B3FB8, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture  // feedingfrenzy-nx: (dev, sampler, texture) before each sprite draw
#if 0  // peggle2-nx / peggle1-nx / bejeweled3-nx: N&B sub_82B96538 (D3DDevice_SetTexture_Inline) is linked in none of Peggle 2, Peggle 1 and Bejeweled 3
BANJO_RENUT_HOOK(sub_82B96538, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline
#endif
BANJO_RENUT_HOOK(sub_820BBD90, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource  // feedingfrenzy-nx: (dev, stream, vb, offset, stride): stride / 4 at +12456 + stream
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices  // N&B sub_8220EE38
BANJO_RENUT_HOOK(sub_820B6160, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration  // feedingfrenzy-nx: (dev, decl): stw +11660, dirty +32 0x480
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // N&B sub_82222EA0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetViewport_hook);  // D3D_SetViewport  // N&B sub_82222F20
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // N&B sub_82223178
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // N&B sub_82223270
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget  // N&B sub_8227AB68
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface  // N&B sub_8227AF40
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // N&B sub_8227AEB0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // N&B sub_8222AA18
#if 0  // bejeweled3-nx: N&B sub_82658678 (D3DDevice_SetCommandBufferPredication) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82446398, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // N&B sub_82658678
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // N&B sub_82220B20
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // N&B sub_8264E2C0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // N&B sub_82658988
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // N&B sub_8264E1A8
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_ResetAllState_hook);  // D3D_ResetAllState  // N&B sub_82287D38
#if 0  // bejeweled3-nx: N&B sub_8264E128 (D3D_InvalidateConstants) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82443FC8, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // N&B sub_8264E128
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // N&B sub_82657488
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk  // N&B sub_8264D8C8
BANJO_RENUT_HOOK(sub_820BBE98, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget  // feedingfrenzy-nx: (dev, index): +12304 + 4 * index, AddRef
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // N&B sub_8264D910
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // N&B sub_8264D840
#if 0  // bejeweled3-nx: N&B sub_82276AC0 (D3DDevice_GetViewport) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82436468, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // N&B sub_82276AC0
#endif
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // N&B sub_8264DAE8
#if 0  // bejeweled3-nx: N&B sub_8264DB38 (D3DDevice_SetVertexFetchConstant) is not linked in Bejeweled 3
BANJO_RENUT_HOOK(sub_82436B20, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // N&B sub_8264DB38
#endif
BANJO_RENUT_HOOK(sub_820B5C78, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // feedingfrenzy-nx: Peggle 1 alignment 1.5; dirty +16
BANJO_RENUT_HOOK(sub_820B5CD0, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // feedingfrenzy-nx: Peggle 1 alignment 1.5; dirty +16
BANJO_RENUT_HOOK(sub_820B5D28, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // feedingfrenzy-nx: Peggle 1 alignment 0.7, shape 0.90
BANJO_RENUT_HOOK(sub_820B5D78, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // feedingfrenzy-nx: Peggle 1 alignment 0.7, shape 0.90
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices  // N&B sub_826561F8

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_820B9EE0, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9F50, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9F10, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9818, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B97E0, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B98F0, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9CC0, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9E18, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9A20, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9AB8, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9988, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9BA8, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9C20, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9B30, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9858, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9D10, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9D68, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9FA8, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820B9FE0, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA058, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA098, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA0D0, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA018, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA1F0, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA218, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA240, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA148, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA188, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA1C0, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA108, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA268, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA290, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA2B8, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA2E8, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA618, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA668, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA5C8, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA6B8, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA488, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA4B8, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA2F8, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA3A0, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA450, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA4F8, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA538, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA578, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA5B8, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAE00, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BADA0, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BADD0, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9A8, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9B8, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9C8, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9D8, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9E8, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BA9F8, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAA08, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // N&B sub_8264CA50
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // N&B sub_8264CA60
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // N&B sub_8264CA70
BANJO_RENUT_HOOK(sub_820BAA18, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAA28, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAA38, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13  // N&B sub_8264CAB0
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14  // N&B sub_8264CAC0
BANJO_RENUT_HOOK(sub_820BAA60, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAAB0, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAB68, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAC18, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BACC8, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAD78, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAE38, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAE70, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAEA0, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAED0, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // N&B sub_8264CED8
BANJO_RENUT_HOOK(sub_820BAF48, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAF78, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BAFA8, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // N&B sub_8264CFA0
BANJO_RENUT_HOOK(sub_820BB008, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB040, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB078, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0A8, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0B8, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB0D0, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable  // N&B sub_8264CFC8
BANJO_RENUT_HOOK(sub_820BAFD8, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable  // feedingfrenzy-nx: state block alignment
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel  // N&B sub_8264C088
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth  // N&B sub_8264C0D8
// feedingfrenzy-nx: not linked or not called in XDK 2571: BANJO_RENUT_HOOK(sub_XXXXXXXX, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames  // N&B sub_8264C130

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_820BB830, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB880, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB8D0, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB7D8, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB330, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB1A0, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB430, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB660, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB708, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB548, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB3E8, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB260, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB4D0, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB780, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB920, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB5B8, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB970, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BB9C0, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BBA10, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // feedingfrenzy-nx: state block alignment
BANJO_RENUT_HOOK(sub_820BBA68, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // feedingfrenzy-nx: state block alignment
