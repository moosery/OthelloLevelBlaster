#include "SortAndDedup.h"
#include <algorithm>

static inline bool BoardKeyLess(const BOARD_KEY& a, const BOARD_KEY& b)
{
    if (a.ullCellsInUse != b.ullCellsInUse) return a.ullCellsInUse < b.ullCellsInUse;
    if (a.ullCellColors != b.ullCellColors) return a.ullCellColors < b.ullCellColors;
    return a.usBoardInfo < b.usBoardInfo;
}

static inline bool BoardKeyEqual(const BOARD_KEY& a, const BOARD_KEY& b)
{
    return a.ullCellsInUse == b.ullCellsInUse
        && a.ullCellColors == b.ullCellColors
        && a.usBoardInfo   == b.usBoardInfo;
}

int SortAndDedup(BOARD_KEY* pBoards, int count)
{
    if (count <= 1) return count;
    std::sort(pBoards, pBoards + count, BoardKeyLess);
    BOARD_KEY* end = std::unique(pBoards, pBoards + count, BoardKeyEqual);
    return (int)(end - pBoards);
}
