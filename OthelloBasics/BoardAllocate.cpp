#include "OthelloBasics.h"
#include "Mem.h"
#include "Error.h"

PBOARD BoardAllocate()
{
    PBOARD pBoard = (PBOARD)MemMalloc("BOARD.boardAllocate", sizeof(BOARD));
    if (pBoard != NULL)
    {
        memset(pBoard, 0, sizeof(BOARD));
    }
    else
    {
        Error(RC_BOARD_ALLOCATE_FAILURE, "BoardAllocate: Failed to allocate memory for a BOARD structure.");
    }

    return pBoard;
}

PBOARD BoardAllocateClone(PBOARD pOrigBoard)
{
    PBOARD pBoard = BoardAllocate();

    if (pBoard != NULL)
    {
        memcpy(pBoard, pOrigBoard, sizeof(BOARD));
    }

    return pBoard;
}

static void BoardSetUpNewBoard(PBOARD pBoard)
{
    /* Set up the board - White upper left and lower right */
    /*                  - Black upper right and lower left */
    /*                  - Black always goes first          */
    SetBoardSizeForRun(g_boardSize);

    SETOCCUPIED(pBoard, 3, 3);
    SETWHITE(pBoard, 3, 3);

    SETOCCUPIED(pBoard, 4, 4);
    SETWHITE(pBoard, 4, 4);

    SETOCCUPIED(pBoard, 3, 4);
    SETBLACK(pBoard, 3, 4);

    SETOCCUPIED(pBoard, 4, 3);
    SETBLACK(pBoard, 4, 3);

    SETBOARDNEXTPLAYER(pBoard, BLACK);

    BoardMoveCalculator(pBoard);
}

/**
    Routine: boardStatsAllocateFirstBoard
    Parameters:
        int boardSize           - The size of the board to create
    Purpose:
     To allocate the first board to play on/resolve.

    Returns:
      PBOARDSTATS - the newly allocated board.  Free with MemFree()!
**/
PBOARD BoardAllocateFirstBoard()
{
    /* Validate the size of the board */
    switch (g_boardSize)
    {
        case(4):
        case(6):
        case(8):
            break;
        default:
            Error(RC_BOARD_INVALID_SIZE,"BoardAllocateFirstBoard: invalid size specified(%d)\n", g_boardSize);
            return NULL;
    }

    /* Create the first boardStats. */
    PBOARD pBoard = BoardAllocate();

    if (pBoard == NULL)
    {
        return NULL;
    }

    /* Set it up */
    BoardSetUpNewBoard(pBoard);

    return(pBoard);
}