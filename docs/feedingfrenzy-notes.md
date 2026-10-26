# Feeding Frenzy port: notes

This file records the port: what each step found, with its evidence. The project is a copy of bejeweled3-nx 1.0.1
(`e877464`): its SDK fork, tools, app and Renut renderer. Identifiers were renamed bejeweled3 -> feedingfrenzy with
`tools/port_rename.py`. Bejeweled 3's notes (`bejeweled3-notes.md`), Peggle 1's and Peggle 2's stay as the reference
for everything taken from those ports; comments that say "bejeweled3-nx", "peggle1-nx" or "Peggle 2" record findings
from those ports.

The user's requests (2026-10-26): "port feeding frenzy 1 to switch", "could we use the new codegen options please"
(futari-nx's VMX keys), and "You don't have to do touch support for this one".

## The game (2026-10-26)

The user's file is `D:\peggle 2\Feeding Frenzy.rar`. It was extracted to `D:\peggle 2\feeding frenzy` and holds one
package: `Feeding Frenzy/584107F1/000D0000/F2727A71...`. That is an XBLA game (content type `0x000D0000`), title
**584107F1**, with no title update and no DLC. `tools/stfs_extract.py` read its 7,812 blocks and extracted 38 entries
(30.4 MiB) to `assets/game_root` (git-ignored).

- **`default.xex`** (`tools/xex_info.py`):
  - Format and layout: XEX2, original PE name `XeFeedingFrenzy.pe`, base `0x82000000`, entry `0x820AD418`, image size
    `0x00370000` (3.4 MB); built 2006-02-28.
  - Identity: version 0.0.1.1, media `1f0f4104`.
  - Imports: `xam.xex` (78) and `xboxkrnl.exe` (210), version 2.0.2571.
  - Static libraries: XDK **2.0.2571** (XAPILIB, D3D9, D3DX9, XGRAPHC, XBOXKRNL, XAUD, XACT, LIBCPMT, XONLINE). That is
    the launch-era XDK: much older than every port before (Peggle 1's 7776, Bejeweled 3's 20764, Peggle 2's 21256).
- **Sections** (the PE header in the loaded image):

  | Section | Range |
  |---|---|
  | `.rdata` | `82000400-8204C9F4` |
  | `.pdata` | `8204CA00-820580F0` |
  | `.text` | `82060000-822E91E4` |
  | `.data` | `822F0000-82333F5C` |
  | `.XEXID` | `82334000` |
  | `.idata` | `82340000` |
  | `.XBLD` | `82350000` |
  | `.reloc` | `82350200-82376434` |
- **Data:** not PopCap's framework (the splash names PopCap and Oberon Media): Xbox packed resources
  (`resources/resources.xpr`, `resources2.xpr`), XACT audio (`resources/sounds/ff_final.xgs`, `Sound Bank.xsb`,
  `Wave Bank.xwb`), splash pictures, the achievement pictures, `ArcadeInfo.xml`.

## Code generation (2026-10-26)

- **The VMX keys** (the user's choice): futari-nx's `out/vmx_exact_codegen.patch` applied to `sdk/` (+257/-8; its
  `config.h` hunk by hand, our fork's options differ there), and its five keys on in `app/perf_overrides.toml`:
  `vmx_exact_minmax`, `vmx_fused_madd`, `vmx_expte_range`, `vmx_cmpb_nan`, `vmx_dot_overflow_qnan`. The generator is
  built from this tree (`out/host`, `sdk/out/win-amd64/rexgluerd.exe`). In the generated code: 71 fused multiply-adds
  (`vmx_madd_ps`), 2 `vmx_max_ps`, 2 `vmx_min_ps`, 3 `vmx_expte_range_ps`.
- **First run:** three "target not in any function", each a tail-call target with no `.pdata` entry reached only from a
  this-adjusting thunk: `0x8226BDE0`, `0x822B4A80` (an AddRef), `0x822B55A8` (stores three vtable words). Declared in
  `app/overrides.toml`. Then 85 files, 9,453 functions, no warnings, 3.6 s.
- **Jump tables** (`tools/switch_check.py --code 82060000:822E91E4`): none truncated, missed or with wrong targets.
  The 13 "table jumps with no generated code" are the optimised codegen's false alarm (Bejeweled 3's notes): checked
  on `sub_8214FB10`'s 300-case table at `0x8214FBE0`, generated as `switch (r27.u32)`.
- **Pointer sweep** (`tools/pointer_fixpoint.py`): 164 functions accepted in pass 1, fixpoint in pass 2; rejected two
  SEH scope labels (`mr r8,r8`) and two tail branches into function bodies. 9,611 functions; the code left in gaps is
  18 KB, 4,016 of the 4,122 gaps being 4-byte alignment padding (the largest 92 bytes).
- **Addresses:**

  | What | Feeding Frenzy | How |
  |---|---|---|
  | setjmp | `0x822D1D30` | hook word `0x82333F4C`, `mflr`, `mfcr`, `stfd f14,0(r3)`; 6 call sites |
  | longjmp | `0x822D1C10` | `lwz r0,312(r3)`, `mr r7,r3`, `lfd f14,0(r7)`; 17 call sites |
  | memset | `0x822CCC40` | `tools/crt_signatures.py scan`: sa2-nx's, unique; 160 bytes, FNV-1a 64 `0x54D201DC8A21DABA` = Peggle 2's `0x825F10B0` |
  | memcpy | `0x822CD480` | 432 call sites; `std r3,-8(r1)`, `dcbt`, aligns to 8; another version than Peggle 2's (1,056 bytes against 1,160); three returns, each `ld r3,-8(r1)`; copies forward |
  | memmove | `0x822CEDF0` | `tools/crt_signatures.py scan`, unique |
  | `D3DDevice_Swap` | `sub_820B1260` | the only caller of `VdSwap` |
  | `_CxxThrowException` | `sub_822CD398` | 20 call sites; copies the 32-byte template at `0x8204A46C` with memcpy |
  | RaiseException | `sub_822DF878` | calls `RtlRaiseException` |
  | entry point | `0x820AD418` | XEX header |
- **Bejeweled 3's game-specific parts, removed or changed** for the first boot:
  - touch controls (`feedingfrenzy_touch.*`, the PC mouse forwarding in `feedingfrenzy_app.h`): not wanted here;
  - the Quest menu fix, the MTRand::SRand test seed (PopCap's framework);
  - the hot-function lists (`localized.txt` 70, `localized_bulk.txt` 319, `inline_hot.txt` 12) and the four
    hand-written natives (`h_*.h`): Bejeweled 3's functions;
  - the memcpy native (another routine here); the memset native kept, at `0x822CCC40`;
  - the Swap hook at `sub_820B1260`; the device's presentation-interval word not found yet (60 fps forcing off);
  - the throw logger at `sub_822CD398`;
  - `tools/inline_memcalls.py` and `tools/leaf_dead_stores.py` take this game's memcpy, memset and entry point.
- Renut's renderer stays off (`FEEDINGFRENZY_RENUT_RENDERER=OFF`) until its hooks are at this game's addresses: the
  first boots use the emulated GPU.
