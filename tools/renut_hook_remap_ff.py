#!/usr/bin/env python3
"""feedingfrenzy-nx (2026-10-26): Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp) at Feeding Frenzy's
addresses (XDK 2.0.2571). One-off, run once on the tree copied from bejeweled3-nx; docs/feedingfrenzy-notes.md,
"Renut's renderer at Feeding Frenzy's addresses", has the evidence.

Feeding Frenzy's game code calls 96 functions of the Direct3D region (out/ff_d3d_api.txt). A PC trace of every call
(app/src/feedingfrenzy_d3d_trace.cpp) told which take the device, their arguments and the order of a frame; each
was then read against Renut's hook of the same name. The render and sampler state functions come from an alignment
of the two libraries' state blocks (out/ff_state_align.txt: the same D3DRS order, fields 96 bytes lower here).

Every hook line whose function is mapped gets this game's address; every other one is commented out: not linked in
this XDK, or not called by the game (Direct3D's internal functions never run while its entry points are hooked).

  python -P tools/renut_hook_remap_ff.py [--dry-run]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')

# Renut hook name (the line's comment) -> Feeding Frenzy address, with the evidence.
API = {
    'Direct3D_CreateDevice': ('820BF218', 'called once with (0, 1, 0, 0, &params, &0x82310EE0); fills the device'),
    'D3DDevice_Clear': ('820B2428', 'once a frame; (dev, count, rects, flags, color, z in f1, -, stencil in r9)'),
    'D3DDevice_Present': ('820B17B0', 'once a frame; calls Swap sub_820B1260 (the only VdSwap caller)'),
    'D3DDevice_SetTexture': ('820B3FB8', '(dev, sampler, texture) before each sprite draw'),
    'D3DDevice_SetPixelShader': ('820B5E18', '(dev, shader): +12712, then the shader+56 literal constants'),
    'D3DDevice_SetVertexShader': ('820B6158', '(dev, shader): stw +13364; b 0x820B6008'),
    'D3DDevice_SetVertexDeclaration': ('820B6160', '(dev, decl): stw +11660, dirty +32 0x480'),
    'D3DDevice_SetStreamSource': ('820BBD90', '(dev, stream, vb, offset, stride): stride / 4 at +12456 + stream'),
    'D3DDevice_DrawVertices': ('820B4118', '(dev, 6 = triangle strip, 0, 4): each sprite'),
    'D3DResource_Release': ('820BCD90', 'decrements Common, destroys at 0'),
    'D3DVertexBuffer_Unlock': ('820BCF18', '(vb) after each sprite\'s vertices'),
    'D3DDevice_CreateTexture': ('820B3D70', '(width, height, depth, levels, usage, format, pool, type 3)'),
    'D3DDevice_CreateVertexBuffer': ('820BCE08', '(length, usage, pool): (4 MB, 8, 0) once'),
    'D3DDevice_CreatePixelShader': ('820B73E8', '(container) -> shader; 56-byte header'),
    'D3DDevice_CreateVertexShader': ('820B74D0', '(container) -> shader; 592-byte header'),
    'D3DDevice_CreateVertexDeclaration': ('820B6180', '(elements) -> declaration; called once'),
    'D3D_Get2DSurfaceDesc_Thunk': ('820B3D50', 'GetLevelDesc (texture, level, desc) before each sprite'),
    'D3DLineTexture_LockRect': ('820B3D58', 'LockRect (texture, level, locked, rect, flags) -> 820B3778'),
    'D3DArrayTexture_UnlockRect': ('820B2570', 'UnlockRect (texture): flushes +28 / +44 pages'),
    'D3DSurface_UnlockRect': ('820B2588', 'the same through the surface\'s texture (+20)'),
    'D3DDevice_GetRenderTarget': ('820BBE98', '(dev, index): +12304 + 4 * index, AddRef'),
    'D3DDevice_SetVertexShaderConstantB': ('820B5C78', 'Peggle 1 alignment 1.5; dirty +16'),
    'D3DDevice_SetPixelShaderConstantB': ('820B5CD0', 'Peggle 1 alignment 1.5; dirty +16'),
    'D3DDevice_SetVertexShaderConstantI': ('820B5D28', 'Peggle 1 alignment 0.7, shape 0.90'),
    'D3DDevice_SetPixelShaderConstantI': ('820B5D78', 'Peggle 1 alignment 0.7, shape 0.90'),
}
# The vertex buffer's Lock returns base + offset here; Renut's N&B hook returns the base: its own adapter.
ADAPTERS = {
    'D3DVertexBuffer_Lock': ('820BCEB8', 'FeedingFrenzyVertexBufferLock'),
}
# State functions: the alignment's pairs at its full score (5.5, or 5.0 for a two-instruction difference), and
# ZEnable / StencilEnable (3.5: they also write the depth-stencil shadow, +11724 / +11728 here, +12284 / +12288 in
# Bejeweled 3, the usual 560 bytes).
STATE_OVERRIDES = {'D3DDevice_SetRenderState_ZEnable': '820B9E98',
                   'D3DDevice_SetRenderState_StencilEnable': '820B9F60'}


def state_map():
    out = {}
    for line in open(os.path.join(ROOT, 'out', 'ff_state_align.txt')):
        m = re.match(r'([0-9A-F]{8})\s+\d+\s+\S+\s+([-\d.]+)\s+(D3DDevice_[GS]et(?:Render|Sampler)State_\w+)', line)
        if m and float(m.group(2)) >= 5.0:
            out[m.group(3)] = m.group(1)
    out.update(STATE_OVERRIDES)
    return out


def main():
    dry = '--dry-run' in sys.argv
    states = state_map()
    mapped = disabled = 0
    for name in sorted(os.listdir(HOOKS)):
        if not name.endswith('.cpp'):
            continue
        path = os.path.join(HOOKS, name)
        text = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        depth = 0  # inside #if 0
        for i, line in enumerate(lines):
            s = line.strip()
            if s.startswith('#if 0'):
                depth += 1
                continue
            if depth and s.startswith('#if'):
                depth += 1
                continue
            if depth and s.startswith('#endif'):
                depth -= 1
                continue
            if depth:
                continue
            m = re.match(r'(\s*)(BANJO_RENUT_HOOK(?:_SHARED)?)\(sub_([0-9A-F]{8}), ([^;]+)\);\s*//\s*(\w+)(.*)$', line)
            if not m:
                continue
            indent, macro, _, target, hname, rest = m.groups()
            addr, why = None, None
            if hname in API:
                addr, why = API[hname]
            elif hname in ADAPTERS:
                addr, target = ADAPTERS[hname]
                why = 'returns base + offset (adapter)'
            elif hname in states:
                addr, why = states[hname], 'state block alignment'
            if hname == 'D3DDevice_Present':
                macro = 'BANJO_RENUT_HOOK_SHARED'  # the frame hook (feedingfrenzy_frame_hooks.cpp) wraps it
            if addr:
                lines[i] = f'{indent}{macro}(sub_{addr}, {target});  // {hname}  // feedingfrenzy-nx: {why}'
                mapped += 1
            else:
                lines[i] = (f'{indent}// feedingfrenzy-nx: not linked or not called in XDK 2571: '
                            f'{macro}(sub_XXXXXXXX, {target});  // {hname}{rest}')
                disabled += 1
        if not dry:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    print(f'renut_hook_remap_ff: {mapped} hooks at Feeding Frenzy\'s addresses, {disabled} commented out'
          + (' (dry run)' if dry else ''))


if __name__ == '__main__':
    main()
