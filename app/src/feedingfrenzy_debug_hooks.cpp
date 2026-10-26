// Temporary hooks that log the callers of functions a boot crashed in (one per finding, each with its evidence), and
// test aids.
//
// bejeweled3-nx (2026-10-21): empty at the start of the port; peggle1-nx's hooks were at Peggle 1's addresses.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

// Test aid (2026-10-21): Bejeweled 3's boards are random (the gems differed between two runs of the same input
// script), which keeps two recordings of the same route (the emulated GPU against Renut's renderer,
// tools/compare_raw.py) from showing the same board. MTRand::SRand(this r3, seed r4) is sub_825286E0 (a seed of 0
// becomes 4357, then the Mersenne Twister's 1812433253 (0x6C078965) recurrence over 624 words; 11 call sites in 9
// functions). With feedingfrenzy_test_seed != 0 every MTRand is seeded with it instead: the same boards in every run.
REXCVAR_DEFINE_UINT32(feedingfrenzy_test_seed, 0, "FeedingFrenzy",
                      "Test runs: every random generator of the game seeded with this value (0 = the game's own seeds)");

// feedingfrenzy-nx (2026-10-26): Feeding Frenzy's random numbers are the CRT's: srand sub_822CCCE0 stores its seed in the
// thread's data (_getptd sub_822CEB30, +20) and rand sub_822CCD10 steps it (`lis 3; ori 0x43FD`: 214013, then
// + 2531011, bits 16-30). With feedingfrenzy_test_seed != 0 every srand gets that seed instead: the same fish in every
// run (the deterministic PC runs that compare the natives with the generated code, docs/feedingfrenzy-notes.md).
extern "C" REX_FUNC(__imp__sub_822CCCE0);
REX_HOOK_RAW(sub_822CCCE0) {  // srand
  if (const uint32_t seed = REXCVAR_GET(feedingfrenzy_test_seed)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[test] srand seeds replaced by feedingfrenzy_test_seed = {} (the game's first was {})", seed,
                  ctx.r3.u32);
    }
    ctx.r3.u64 = seed;
  }
  __imp__sub_822CCCE0(ctx, base);
}
