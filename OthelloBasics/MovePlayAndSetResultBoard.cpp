#include "OthelloBasics.h"
#include <memory.h>

#define BITBOARD_FLIP

#ifdef BITBOARD_FLIP

// Column masks for wrap prevention (full 8x8 grid)
static const unsigned long long NOT_LEFT_COL  = ~0x8080808080808080ULL; // ~col 0
static const unsigned long long NOT_RIGHT_COL = ~0x0101010101010101ULL; // ~col 7

static unsigned long long computeFlips(unsigned long long moveBit,
                                       unsigned long long player,
                                       unsigned long long opponent)
{
    unsigned long long flips = 0, x;

    // Up (<<8)
    x  = (moveBit << 8) & opponent;
    x |= (x      << 8) & opponent;
    x |= (x      << 8) & opponent;
    x |= (x      << 8) & opponent;
    x |= (x      << 8) & opponent;
    x |= (x      << 8) & opponent;
    if ((x       << 8) & player) flips |= x;

    // Down (>>8)
    x  = (moveBit >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    if ((x       >> 8) & player) flips |= x;

    // Right (>>1) — mask col 7 before each shift to prevent row wrap
    x  = ((moveBit & NOT_RIGHT_COL) >> 1) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 1) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 1) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 1) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 1) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 1) & opponent;
    if   ((x       & NOT_RIGHT_COL) >> 1  & player) flips |= x;

    // Left (<<1) — mask col 0 before each shift to prevent row wrap
    x  = ((moveBit & NOT_LEFT_COL) << 1) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 1) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 1) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 1) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 1) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 1) & opponent;
    if   ((x       & NOT_LEFT_COL) << 1  & player) flips |= x;

    // Down-Right (>>9) — mask col 7
    x  = ((moveBit & NOT_RIGHT_COL) >> 9) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 9) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 9) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 9) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 9) & opponent;
    x |= ((x       & NOT_RIGHT_COL) >> 9) & opponent;
    if   ((x       & NOT_RIGHT_COL) >> 9  & player) flips |= x;

    // Down-Left (>>7) — mask col 0
    x  = ((moveBit & NOT_LEFT_COL) >> 7) & opponent;
    x |= ((x       & NOT_LEFT_COL) >> 7) & opponent;
    x |= ((x       & NOT_LEFT_COL) >> 7) & opponent;
    x |= ((x       & NOT_LEFT_COL) >> 7) & opponent;
    x |= ((x       & NOT_LEFT_COL) >> 7) & opponent;
    x |= ((x       & NOT_LEFT_COL) >> 7) & opponent;
    if   ((x       & NOT_LEFT_COL) >> 7  & player) flips |= x;

    // Up-Right (<<7) — mask col 7
    x  = ((moveBit & NOT_RIGHT_COL) << 7) & opponent;
    x |= ((x       & NOT_RIGHT_COL) << 7) & opponent;
    x |= ((x       & NOT_RIGHT_COL) << 7) & opponent;
    x |= ((x       & NOT_RIGHT_COL) << 7) & opponent;
    x |= ((x       & NOT_RIGHT_COL) << 7) & opponent;
    x |= ((x       & NOT_RIGHT_COL) << 7) & opponent;
    if   ((x       & NOT_RIGHT_COL) << 7  & player) flips |= x;

    // Up-Left (<<9) — mask col 0
    x  = ((moveBit & NOT_LEFT_COL) << 9) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 9) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 9) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 9) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 9) & opponent;
    x |= ((x       & NOT_LEFT_COL) << 9) & opponent;
    if   ((x       & NOT_LEFT_COL) << 9  & player) flips |= x;

    return flips;
}

static void applyMove(PBOARD pBoard, char color, int row, int col)
{
    unsigned long long moveBit  = FIRSTBIT >> GETINDEX(row, col);
    unsigned long long occupied = pBoard->ullCellsInUse;
    unsigned long long colors   = pBoard->ullCellColors;

    unsigned long long player, opponent;
    if (color == BLACK)
    {
        player   = occupied & colors;
        opponent = occupied & ~colors;
    }
    else
    {
        player   = occupied & ~colors;
        opponent = occupied & colors;
    }

    unsigned long long flips = computeFlips(moveBit, player, opponent);

    pBoard->ullCellsInUse |= moveBit;
    if (color == BLACK)
        pBoard->ullCellColors |= (moveBit | flips);
    else
        pBoard->ullCellColors &= ~(moveBit | flips);
}

#else // !BITBOARD_FLIP

static bool moveCheckDir(PBOARD pBoard, char color, int row, int col,
                         int rowDir, int colDir, int si, int ei)
{
    int foundOppositeColor = 0;
    int foundSameColor = 0;
    char cellColor;

    while (!foundSameColor)
    {
        row += rowDir;
        col += colDir;

        if (row >= si && row < ei)
        {
            if (col >= si && col < ei)
            {
                if (!ISOCCUPIED(pBoard, row, col))
                {
                    break;
                }
                else
                {
                    cellColor = GETCOLOR(pBoard, row, col);

                    if (cellColor == color)
                    {
                        foundSameColor = true;
                        break;
                    }
                    else
                    {
                        foundOppositeColor = true;
                    }
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    if (foundOppositeColor && foundSameColor)
    {
        return true;
    }

    return false;
}

static void moveDir(PBOARD pBoard, char color, int row, int col,
                    int rowDir, int colDir, int si, int ei)
{
    while (true)
    {
        row += rowDir;
        col += colDir;

        if (row >= si && row < ei)
        {
            if (col >= si && col < ei)
            {
                if (GETCOLOR(pBoard, row, col) == color)
                {
                    break;
                }
                else
                {
                    SETCOLOR(pBoard, row, col, color);
                }
            }
        }
    }
}

static void applyMove(PBOARD pBoard, char color, int row, int col)
{
    const int si = g_boardSi;
    const int ei = g_boardEi;

    SETOCCUPIED(pBoard, row, col);
    SETCOLOR(pBoard, row, col, color);

    if (moveCheckDir(pBoard, color, row, col, -1,  0, si, ei))
        moveDir(pBoard, color, row, col, -1,  0, si, ei);
    if (moveCheckDir(pBoard, color, row, col, -1,  1, si, ei))
        moveDir(pBoard, color, row, col, -1,  1, si, ei);
    if (moveCheckDir(pBoard, color, row, col,  0,  1, si, ei))
        moveDir(pBoard, color, row, col,  0,  1, si, ei);
    if (moveCheckDir(pBoard, color, row, col,  1,  1, si, ei))
        moveDir(pBoard, color, row, col,  1,  1, si, ei);
    if (moveCheckDir(pBoard, color, row, col,  1,  0, si, ei))
        moveDir(pBoard, color, row, col,  1,  0, si, ei);
    if (moveCheckDir(pBoard, color, row, col,  1, -1, si, ei))
        moveDir(pBoard, color, row, col,  1, -1, si, ei);
    if (moveCheckDir(pBoard, color, row, col,  0, -1, si, ei))
        moveDir(pBoard, color, row, col,  0, -1, si, ei);
    if (moveCheckDir(pBoard, color, row, col, -1, -1, si, ei))
        moveDir(pBoard, color, row, col, -1, -1, si, ei);
}

#endif // BITBOARD_FLIP

void MovePlayAndSetResultBoard(PBOARD pBoard, PBOARD pResultBoard, int row, int col)
{
	memset(pResultBoard, 0, sizeof(BOARD));

	pResultBoard->ullCellsInUse = pBoard->ullCellsInUse;
	pResultBoard->ullCellColors = pBoard->ullCellColors;
	pResultBoard->usBoardInfo   = pBoard->usBoardInfo;

	SETBOARDNEXTPLAYERFLIP(pResultBoard);

    char color = GETBOARDNEXTPLAYER(pBoard);

    applyMove(pResultBoard, color, row, col);
}
