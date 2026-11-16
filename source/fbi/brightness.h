#pragma once

#include <3ds.h>

// The console's brightness range (issue #42): its five levels, kept in the system configuration (block 0x00050002, read
// by GSP), spread between a lowest and a highest brightness the user picks. Auto-brightness follows the same table: its
// first value is how dark it gets, its fifth how bright. It works in every game and in HOME, and stays after NSA closes.
// The console's own table is kept in sdmc:/nsa/brightness-factory.bin before the first change; "System" puts it back.

// The ends of the bars, in the table's own unit (the console's levels are 16, 28, 48, 82 and 142).
#define BRIGHTNESS_LOWEST 10
#define BRIGHTNESS_HIGHEST 172

// The table can be read and has the layout NSA knows (five levels).
bool brightness_supported();

// The console's levels now, and the console's own ones (from the backup, else the usual ones).
bool brightness_levels(u16* levels);
void brightness_factory_levels(u16* levels);

// The levels in use are not the console's own.
bool brightness_custom();

// The five levels between lowest and highest (lowest <= highest), each a step further by the same ratio.
void brightness_spread(u32 lowest, u32 highest, u16* levels);

// Writes the levels for this range and has GSP take them at once (manual and auto). save: also into the configuration
// on the NAND, so they stay (a slider that moves only sets them; it saves when the finger lifts).
Result brightness_set(u32 lowest, u32 highest, bool save);

// The console's own levels again, saved.
Result brightness_restore();

// The brightness the top screen has now, or 0 when the console cannot tell (an Old 3DS).
u32 brightness_now();
