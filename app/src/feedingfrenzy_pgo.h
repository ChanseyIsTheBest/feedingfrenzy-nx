// Peggle: profile collection of the PGO-instrumented Switch build (FEEDINGFRENZY_PGO=generate), see feedingfrenzy_pgo.cpp.
// In every other build these do nothing. From sa2-nx (sa2_pgo.h).

#pragma once

namespace feedingfrenzy::pgo {

// Picks this session's file (pgo/feedingfrenzy_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace feedingfrenzy::pgo
