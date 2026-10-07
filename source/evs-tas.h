/*
 * evs-tas.h - Everscript input recording / replay support
 *
 * Compile-time switch, constant on. Build with -DEVS_TAS=0 to get the
 * original snes9x2005 behaviour back:
 *   - startWithRom on a running core: plain S9xReset (instead of a power-on)
 *   - debugger readMemory/readMemoryRange: through S9xGetByte (instead of a
 *     side-effect-free peek)
 *   - no lag-frame flag (takeInputPolled)
 */

#ifndef EVS_TAS_H
#define EVS_TAS_H

#ifndef EVS_TAS
#define EVS_TAS 1
#endif

#if EVS_TAS
#include <stdbool.h>
/* The game read a joypad register since the host last asked (ppu.c). */
extern bool evsInputPolled;
#endif

#endif
