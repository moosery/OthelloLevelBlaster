#include "OthelloBasics.h"

int BoardCompare(const void* arg1, const void* arg2)
{
    int result;

    PBOARD pBoard1 = (PBOARD)arg1;
    PBOARD pBoard2 = (PBOARD)arg2;

    if (pBoard1->ullCellsInUse < pBoard2->ullCellsInUse)
        result = -1;
    else if (pBoard1->ullCellsInUse == pBoard2->ullCellsInUse)
    {
        if (pBoard1->ullCellColors < pBoard2->ullCellColors)
            result = -1;
        else if (pBoard1->ullCellColors == pBoard2->ullCellColors)
        {
            if (GETBOARDNEXTPLAYER(pBoard1) == GETBOARDNEXTPLAYER(pBoard2))
                result = 0;
            else if (GETBOARDNEXTPLAYER(pBoard1) < GETBOARDNEXTPLAYER(pBoard2))
                result = -1;
            else
                result = 1;
        }
        else
            result = 1;
    }
    else
        result = 1;

    return result;
}

int BoardCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size)
{
    return BoardCompare(arg1, arg2);
}
