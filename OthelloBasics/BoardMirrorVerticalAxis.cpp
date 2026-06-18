#include "OthelloBasics.h"

/* Define BITBOARD_MIRROR to use the bitwise implementation (default).
** Comment it out to use the original cell-by-cell implementation.
*/
#define BITBOARD_MIRROR

#ifdef BITBOARD_MIRROR

// Reverse bits within each byte of a 64-bit word independently.
// Mirroring the vertical axis = reversing column order within every row.
// Each row of the board occupies exactly one byte in the bitfield, so a
// per-byte bit-reversal is all that is needed.  Non-active column bits are
// always 0, so swapping them with their mirrors leaves them 0 — this works
// unchanged for 4x4, 6x6, and 8x8 boards with no size-specific branching.
// Three passes of paired swaps (nibbles → bit-pairs → adjacent bits) fully
// reverse the 8 bits of each byte.
static unsigned long long mirrorBytewise(unsigned long long x)
{
    x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((x & 0x5555555555555555ULL) << 1);
    return x;
}

void BoardMirrorVerticalAxis(PBOARD pBoard, PBOARD pResult)
{
    pResult->usBoardInfo      = pBoard->usBoardInfo;
    pResult->_pad1[0] = pResult->_pad1[1] = pResult->_pad1[2] = 0;
    pResult->ullCellsInUse    = mirrorBytewise(pBoard->ullCellsInUse);
    pResult->ullCellColors    = mirrorBytewise(pBoard->ullCellColors);
    pResult->ullPossibleMoves = mirrorBytewise(pBoard->ullPossibleMoves);
    pResult->_pad2[0] = pResult->_pad2[1] = pResult->_pad2[2] = 0;
}

#else

void BoardMirrorVerticalAxis(PBOARD pBoard, PBOARD pResult)
{
    int startIdx = g_boardSi;
    int endIdx = g_boardEi;

    pResult->usBoardInfo = pBoard->usBoardInfo;
    pResult->_pad1[0] = pResult->_pad1[1] = pResult->_pad1[2] = 0;
    pResult->ullPossibleMoves = 0;
    pResult->_pad2[0] = pResult->_pad2[1] = pResult->_pad2[2] = 0;

    for (int row = startIdx; row < endIdx; row++)
    {
        for (int col = startIdx, newCol = (endIdx - 1); col < endIdx; col++, newCol--)
        {
            if (ISOCCUPIED(pBoard, row, col))
            {
                SETOCCUPIED(pResult, row, newCol);
                SETCOLOR(pResult, row, newCol, GETCOLOR(pBoard, row, col));
            }

            if (ISPOSSIBLE(pBoard, row, col))
                SETPOSSIBLE(pResult, row, newCol);
        }
    }
}

#endif
