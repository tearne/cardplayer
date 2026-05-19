#pragma once

// Fuzzy track-finding investigation harness.
//
// Compiled in only when FUZZY_HARNESS is defined at build time. The harness
// adds no code or data when the flag is off — header inlines collapse to
// no-ops and the .cpp body is wrapped in #ifdef.
//
// When enabled, the firmware boots into the regular UI/audio paths *and*
// listens on the USB serial port for harness commands. See fuzzy_harness.cpp
// for the command vocabulary.

#ifdef FUZZY_HARNESS
void fuzzyHarnessSetup();
void fuzzyHarnessPoll();
#else
inline void fuzzyHarnessSetup() {}
inline void fuzzyHarnessPoll() {}
#endif
