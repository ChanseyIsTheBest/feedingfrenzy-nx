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

## First boots on the PC (2026-10-26, emulated GPU)

- The first build failed in the post-codegen passes (they rewrote files while the build compiled them, Bejeweled 3's
  "build loop" race); `tools/win/bj3_regen_build.ps1` (codegen target first, then the rest) builds it.
- `boot1`: the Sprout Games / Oberon Media splash, the title ("PRESS A TO START") with the fish swimming, 60 game frames a
  second, the XMA audio decoding, the generated-code self-checks 0 differences.
- `play1` (pad script): the main menu (New Game, Time Attack, Leaderboards, Achievements, Help & Options, Return to
  Arcade), Stage 1's introduction, play (eating fish: "Yummy!", being eaten: "SORRY"). The log has no unimplemented call;
  only the splash variants the game tries first (`hd_gameSplash_en.jpg` and others) are missing, as on the 360.

## Renut's renderer at Feeding Frenzy's addresses (2026-10-26)

The user chose Renut on deko3d for the Switch (over the emulated GPU with the NVK Vulkan driver).

- **What the game calls** (`out/ff_d3d_api.txt`): the Direct3D library is about `0x820AE000`-`0x820CA000` (packet
  builders, the 117 small state functions at `0x820BA000`); game code calls 96 of its 570 functions.
- **The trace** (`app/src/feedingfrenzy_d3d_trace.cpp`, PC test aid, off by default; the build leaves it out when Renut's
  renderer is in): every call of the 96 up to a game frame, whether r3 was the device (the word at `0x82310EE0`,
  `Direct3D_CreateDevice`'s output), the first arguments, the order within a frame. The device is created by
  `sub_820AADF8` (`CreateDevice(0, 1, 0, 0, &params, &0x82310EE0)`, then Clear and Present); a frame is Clear, about 80
  sprites, Present. A sprite (game code around `0x820A59D8`): GetLevelDesc of its texture, the vertex buffer's Lock of
  0x90 bytes (4 vertices of 36: position, two colors, two texture coordinates), Unlock, SetVertexDeclaration,
  SetStreamSource, SetTexture, DrawVertices(triangle strip, 0, 4), SetTexture(1, NULL), the shaders. Creation functions take
  no device (CreateTexture 417 times, each locked, filled and unlocked; one 4 MB vertex buffer; one declaration).
- **States:** Feeding Frenzy's state block is Bejeweled 3's in the same D3DRS order (`out/ff_state_align.txt`, a
  Needleman-Wunsch alignment on what each function writes): 198 pairs at the full score; the packet fields 96 bytes lower
  here, the shadows 560. XDK 2571 lacks LastPixel, LineWidth, Buffer2Frames and HiZEnable / HiZWriteEnable at that place.
- **The device's front is N&B's layout:** the game writes sampler fields of the texture fetch constants itself
  (`device+0x480`, dirty bits in the qword at +16), and its SetVertexShaderConstantF (`sub_820B59E8`) writes `+0x780` and
  the dirty mask at +0: where Renut's device keeps them. So those stay the game's own code.
- **Signatures checked one by one** against Renut's hooks: Clear (Z in f1, stencil in r9), SetStreamSource (stride / 4
  stored), CreateTexture (width, height, depth, levels, usage, format, pool, type 3), CreateVertexBuffer, the shader
  creators (one argument: the container), CreateVertexDeclaration (`sub_820B6180`; `sub_820B8608` is the shader
  builder's 9,808-byte object), GetLevelDesc = Renut's Get2DSurfaceDesc, LockRect = D3DLineTexture_LockRect's
  arguments. **The vertex buffer's Lock returns base + offset** here (N&B's returns the base): an adapter.
- **The table** (`tools/renut_hook_remap_ff.py`): 225 hooks at this game's addresses, 99 commented out. Present is the
  shared hook: the frame hook (frame counter, captures, pacing) is on `sub_820B17B0` and calls Renut's Present.
- **Renut's guest data:** the XDK build qword `0x822F20D8` (`0x000200000A0B4001`), the `VdGlobalDevice` import slot
  `0x82000574`, the caps `0x8200C8D0`, the format table `0x8200C670` (Bejeweled 3's 128 bytes); the state defaults as host
  constants (from Bejeweled 3's tables: XDK 2571's are not N&B's order); the HiZ owner a host variable; no export
  switches, no "no large pages" flag; the full viewport passed as values. GetCurrentThreadId `sub_822A5FA0` and XMemFree
  `sub_820AD808` (the same code as Bejeweled 3's).
- **Shaders:** the game compiles its 5 shaders at start (3 pixel, 2 vertex) with XDK 2571's compiler, which writes
  containers of version **0x0E**: a 0x18-byte header (flags, virtual size, physical size, definition table, constant
  table, shader info), the shader info one word shorter in front (interpolator info at +16, not +20; vertex shaders an extra
  word after field20), literals inline in the definition table. `XenosRecomp/container_0e.h` turns each into 0x11 under
  its own hash; Renut accepts both. The literal's register field is (register + 48) << 4: the vertex shaders' `0x12F0` is
  c255 (`oPos.xy = position + c255.x`, the half-pixel -0.5).
- **PC, Renut on Vulkan** (`renut2`): the splash, title, menu, Stage 1, play, the HUD and the score as with the emulated
  GPU.

## The first Switch build (2026-10-26)

- **Shaders:** XenosRecomp with `BANJO_HLSL_OVERRIDE=out/pc/renut_gpu/early_alpha_hlsl` and `BANJO_DEKO3D_SPIRV_DIR`
  (the 5 containers to SPIR-V), `deko3d_shaders.py` (5 of 5 compile with uam: 2 vertex of 2 KB, 3 pixel of 1 KB),
  `deko3d_glsl_opt.py`, `deko3d_shader_package.py` (22 KB), `renut_switch_shader_cache.py`. All five are the game's
  run-time shaders, so the NRO carries them all (none is stored in default.xex). The deko3d PC tools (SPIRV-Cross, uam)
  were copied from bejeweled3-nx's `out/deko3d_tools`.
- **Build:** `tools/win/build_switch.ps1 -Pgo none -Define "FEEDINGFRENZY_SWITCH_LTO=OFF"`. The first failed on the
  built-in config's raw string delimiter (`FEEDINGFRENZYCONFIG`, 19 characters; C++ allows 16): now `FFCONFIG`. NRO
  22.6 MB, "Feeding Frenzy" 0.1.0 with the game's 64x64 `title_image.png` as the development icon (`out/icon_dev.jpg`).
- **`tools/check_switch_build.py`:** all checks passed after:
  - KNOWN_HALTS: memcpy `sub_822CD480` (three `b .` in XDK 2571's CRT copy, each after a `bdnz` whose loop leaves
    through the `bge` before it);
  - STREAMING_COPIES: none (no function has the 72-vperm-and-lvsl shape);
  - the vector fused multiply-adds of `vmx_fused_madd`: 71 in 14 functions, exactly the generated code's `vmx_madd_ps`
    sites (`FusedMaddFunctions`: the functions with the helper or an inlined copy of one).
- **Ryujinx** (`emu1`; SD `switch\feedingfrenzy-nx` a junction to `out/emu_sd/feedingfrenzy-nx` in the Peggle data
  folder; `tools/build_asset_pack.py` made the game pack, 32 files, 30 MB): splash, title, menu, Stage 1, play (score
  20), 60.00 game frames a second, Renut's render thread 1.42 ms a frame, no shader missing, no crash in 150 s.
- **Delivered:** `out/dist/dev_20261026_feedingfrenzy/feedingfrenzy-nx/` (NRO `379533c7...`, the game pack, README.txt);
  ELF and map `out/nx/elf/feedingfrenzy_dev_20261026_1458.*`.

## Optimising the hot functions (2026-10-26, from the emulator)

The user: "do the bejeweled 3 optimising functions guide, just use data from the emulator: hand write a handful of the
top functions, machine optimise around the top 100, and do the register write-back stuff"; then "64 functions is
enough". The method is docs/optimising-functions.md.

- **Profile** (Ryujinx `prof1`: 300 s of Stage 1 with the stick, `switch_profile_report` / `switch_profile_stacks`;
  `out/renut_tests/emu/prof1/rex_profile.log`, map `out/nx/elf/feedingfrenzy_dev_20261026_1458.map`): the main thread
  is about 20 % of a core at 60 fps (it waits for the frame most of the time). 64 generated functions were sampled at
  all (all guest threads; `tools/switch_hot_functions.py --top 5000`):
  - `sub_822CCD58`, the CRT **strstr** (a byte-by-byte search), 39 % of the main thread's busy samples;
  - `sub_82256198`, a game method that ORs each child's flags (+112) into its own over a vector at +296 (the debug
    iterator's checks still in it), 21 %;
  - the rest 2.4 % and less each (`_stricmp` sub_822CFFE0, memcpy sub_822CD480, memmove sub_822CEDF0 among them).
  A second, longer run (`prof2`, Time Attack) stopped before the first frame in Ryujinx (the guest never presented; the
  same NRO started twice before and since): watched for in the later runs.
- **Hand-written natives** (five):
  - the CRT's strstr (`n_822CCD58.h`: the host's strstr; an empty needle returns r3 as it came), _stricmp
    (`n_822CFFE0.h`: written out, because this CRT returns the first string's byte *before* lower-casing when the second
    ends: "A" against "" gives 65), memmove (`n_822CEDF0.h`: the host's memmove; overlapping ranges on both sides of
    0x80000000 declined, the original picks its direction by a signed compare) and memcpy (sub_822CD480 with
    n_825F0AE0.h's native: the same contract, checked: r3 saved at -8(r1), a forward copy, no dcbz, 32-bit sizes),
    registered with their sizes and FNV-1a 64 hashes in `feedingfrenzy_hot_guest.cpp` and listed in `hand.txt`;
  - `h_82256198.h`: the vector's begin / end loaded once (the loop's one store, +112, cannot reach +300 / +304), the
    running OR kept in a register (still stored every iteration), the one-instruction callee inline; every effect of
    the copy audited in the header.
- **Localized copies:** the other 60 sampled functions (35 in `localized.txt`, 25 in `localized_bulk.txt`); 12 have
  small callees inline. **Write-backs of the changed registers only** (tools/hand_native_dirty_sync.py, in every copy):
  1,427 register stores kept of 2,839 (49 % removed); smart sync at 16 of 104 call sites (84 write-back / reload pairs
  fewer). `sub_820B02F4` (an SEH funclet: an unrecognized use of the XER local) keeps all of its write-backs.
- **Checks:**
  - PC verify mode (`verify1`, Renut, play): 20,567,779 checked calls of the leaf copies and the CRT natives, 0
    differences. In 10 s: strstr 486,144 calls, memmove 355,840, memcpy 72,192, `h_82256198` 913,920.
  - The deterministic PC comparison (Bejeweled 3's check of the natives with calls) does not work in this game:
    `tools/win/ff_lockstep_check.ps1` (the test seed on the CRT srand sub_822CCCE0, the frame clock, the pad script)
    gives two baseline runs without any reproducible frame. The guest time is the frame clock's (mftb reads
    QueryGuestTickCount), but the game loads its data on another thread, so the number of frames the loading takes
    varies with the PC's disk timing, and the frame-timed presses land on other screens (`ta` / `tb`: at frame 900 one
    run is still at "PRESS A TO START", the other in the menu). The copies with calls are the generator's code
    (exact by construction, as in Bejeweled 3); `h_82256198`, the one hand-written native with a call, is audited in its
    header.
- **Measured** (Ryujinx `prof3`: the optimised NRO, `prof1`'s script and length): strstr's share fell from 886 samples
  to 252 (the host's strstr 30, with the memcmp 187 and strlen 35 it calls); `h_82256198` 575 samples where the copy-less
  run had 480, while the unchanged-shape functions around it rose 1.5-2.5x: the run's workload was heavier (it reached
  level changes: host memmove 338 and memset 246 samples, the guest's memmove had 20). Whole-frame times of the two runs
  are not comparable (the game's scenes differ between runs; see the deterministic check above).
- Delivered: the optimised NRO in `out/dist/dev_20261026_feedingfrenzy/feedingfrenzy-nx/`; ELF and map
  `out/nx/elf/feedingfrenzy_dev_opt_20261026.*`.
