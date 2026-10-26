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

// feedingfrenzy-nx (2026-10-26): Bejeweled 3's MTRand::SRand hook removed; Feeding Frenzy's random generator is
// not found yet, so feedingfrenzy_test_seed does nothing for now.
