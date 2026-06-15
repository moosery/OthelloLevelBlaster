#pragma once
#include "OthelloTypes.h"

// Writes the standard Othello starting position as a single BOARD_KEY record to
// storeDirectory\Level_0000_file_0000.bin.  No-ops if the file already exists
// (supports --restart).  Fatals on any I/O failure.
void CreateSeedFile(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState);
