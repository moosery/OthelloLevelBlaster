#include "OthelloBasics.h"
#include <string.h>
#include <stdint.h>

int BoardKeyCompare(const void* arg1, const void* arg2)
{
    const BOARD_KEY* a = (const BOARD_KEY*)arg1;
    const BOARD_KEY* b = (const BOARD_KEY*)arg2;
    if (a->ullCellsInUse != b->ullCellsInUse)
        return (a->ullCellsInUse < b->ullCellsInUse) ? -1 : 1;
    if (a->ullCellColors != b->ullCellColors)
        return (a->ullCellColors < b->ullCellColors) ? -1 : 1;
    // Compare bytes 16-23 as uint64_t to match GPU radix sort field f2
    uint64_t af2, bf2;
    memcpy(&af2, &a->usBoardInfo, sizeof(uint64_t));
    memcpy(&bf2, &b->usBoardInfo, sizeof(uint64_t));
    if (af2 != bf2)
        return (af2 < bf2) ? -1 : 1;
    return 0;
}

int BoardKeyCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size)
{
    return BoardKeyCompare(arg1, arg2);
}
