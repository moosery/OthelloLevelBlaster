#pragma once
#include "OthelloBasics.h"

// Sort pBoards[0..count) in-place by (ullCellsInUse, ullCellColors, usBoardInfo)
// and compact duplicate entries.
// Returns the number of unique boards remaining in pBoards[0..returnValue).
int SortAndDedup(BOARD_KEY* pBoards, int count);
