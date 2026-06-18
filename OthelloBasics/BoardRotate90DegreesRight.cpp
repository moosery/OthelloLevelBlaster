#include "OthelloBasics.h"
#include <stdlib.h>

#ifndef USE_ORIGINAL_ROTATION

/* Transpose an 8x8 bit matrix stored in a uint64_t (MSB = row 0, col 0).
   Uses the standard diagonal-flip algorithm from chess programming literature. */
static inline unsigned long long flipDiagA1H8(unsigned long long x)
{
    unsigned long long t;
    const unsigned long long k1 = 0x5500550055005500ULL;
    const unsigned long long k2 = 0x3333000033330000ULL;
    const unsigned long long k4 = 0x0F0F0F0F00000000ULL;
    t  = k4 & (x ^ (x << 28)); x ^= t ^ (t >> 28);
    t  = k2 & (x ^ (x << 14)); x ^= t ^ (t >> 14);
    t  = k1 & (x ^ (x << 7));  x ^= t ^ (t >> 7);
    return x;
}

/* 90 degrees clockwise = reverse rows (byteswap) then transpose.
   Works correctly for all board sizes (4x4, 6x6, 8x8) because
   the centered sub-board is self-contained under full-word rotation. */
void BoardRotate90DegreesRight(PBOARD pBoard, PBOARD pResult)
{
    pResult->usBoardInfo      = pBoard->usBoardInfo;
    pResult->_pad1[0] = pResult->_pad1[1] = pResult->_pad1[2] = 0;
    pResult->ullPossibleMoves = 0;
    pResult->ullCellsInUse    = flipDiagA1H8(_byteswap_uint64(pBoard->ullCellsInUse));
    pResult->ullCellColors    = flipDiagA1H8(_byteswap_uint64(pBoard->ullCellColors));
    pResult->_pad2[0] = pResult->_pad2[1] = pResult->_pad2[2] = 0;
}

#else /* USE_ORIGINAL_ROTATION */

void BoardRotate90DegreesRight(PBOARD pBoard, PBOARD pResult)
{
    int startIdx = g_boardSi;
    int endIdx = g_boardEi;
    int newRow;
    int newCol;

    pResult->usBoardInfo = pBoard->usBoardInfo;
    pResult->_pad1[0] = pResult->_pad1[1] = pResult->_pad1[2] = 0;
    pResult->ullPossibleMoves = 0;
    pResult->ullCellColors = 0;
    pResult->ullCellsInUse = 0;
    pResult->_pad2[0] = pResult->_pad2[1] = pResult->_pad2[2] = 0;

    for (int row = startIdx; row < endIdx; row++)
    {
        for (int col = startIdx; col < endIdx; col++)
        {
            newRow = col;
            newCol = 7 - row;

            if (ISOCCUPIED(pBoard, row, col))
            {
                SETOCCUPIED(pResult, newRow, newCol);
                SETCOLOR(pResult, newRow, newCol, GETCOLOR(pBoard, row, col));
            }

            if (ISPOSSIBLE(pBoard, row, col))
                SETPOSSIBLE(pResult, newRow, newCol);
        }
    }
}

#endif /* USE_ORIGINAL_ROTATION */
