#include "OthelloBasics.h"
#include "Mem.h"
#include "Error.h"

PMOVE MoveAllocate()
{
    PMOVE pMove = (PMOVE)MemMalloc("MOVE.moveAllocate", sizeof(MOVE));
    if (pMove != NULL)
    {
        memset(pMove, 0, sizeof(MOVE));
    }
    else
    {
        Error(RC_BOARD_MOVE_ALLOCATE_FAILURE, "MoveAllocate: Failed to allocate memory for a MOVE structure.");
    }

    return pMove;
}

void MoveSet(PMOVE pMove, PBOARD pParent, PBOARD pResult, unsigned short usMoveIdx)
{
    memset(pMove, 0, sizeof(MOVE));
    pMove->usMoveIdx = usMoveIdx;
    pMove->ullCellsInUseParent = pParent->ullCellsInUse;
    pMove->ullCellColorsParent = pParent->ullCellColors;
    pMove->usBoardInfoParent = pParent->usBoardInfo;
    pMove->ullCellsInUseResult = pResult->ullCellsInUse;
    pMove->ullCellColorsResult = pResult->ullCellColors;
    pMove->usBoardInfoResult = pResult->usBoardInfo;
}