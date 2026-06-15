#include "OthelloBasicsForCUDA.h"

DevBoardConsts OBCuda_GetBoardConsts()
{
    DevBoardConsts c;
    c.boardMask      = g_boardMask;
    c.boardRightEdge = g_boardRightEdge;
    c.boardLeftEdge  = g_boardLeftEdge;
    return c;
}
