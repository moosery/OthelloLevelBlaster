#include "OthelloBasics.h"

void BoardFlip(PBOARD pBoard, PBOARD pResult)
{
    pResult->usBoardInfo = pBoard->usBoardInfo;
    pResult->_pad1[0] = pResult->_pad1[1] = pResult->_pad1[2] = 0;
    pResult->ullPossibleMoves = 0;
    pResult->ullCellColors = 0;
    pResult->ullCellsInUse = 0;
    pResult->_pad2[0] = pResult->_pad2[1] = pResult->_pad2[2] = 0;

    if (GETBOARDNEXTPLAYER(pBoard) == BLACK)
    {
        SETBOARDNEXTPLAYER(pResult, WHITE);
    }
    else
    {
        SETBOARDNEXTPLAYER(pResult, BLACK);
    }

    for (int row = g_boardSi; row < g_boardEi; row++)
    {
        for (int col = g_boardSi; col < g_boardEi; col++)
        {
            if (ISOCCUPIED(pBoard, row, col))
            {
                SETOCCUPIED(pResult, row, col);
                char color = (GETCOLOR(pBoard, row, col) == BLACK ? WHITE : BLACK);
                SETCOLOR(pResult, row, col, color);
            }

            if (ISPOSSIBLE(pBoard, row, col))
                SETPOSSIBLE(pResult, row, col);

        }
    }
}
