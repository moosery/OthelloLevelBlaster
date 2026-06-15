#include "CreateSeedFile.h"
#include "BlasterFile.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>

void CreateSeedFile(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState)
{
    (void)pConfig;

    char seedPath[MAX_FULL_PATH_NAME];
    snprintf(seedPath, sizeof(seedPath), "%s\\Level_0000_file_0000.bin",
             pState->storeDirectory);

    // Already exists and is valid — restart scenario, nothing to do
    BLFReader* r = BLFOpen(seedPath);
    if (r)
    {
        uint64_t count = BLFTrailer(r)->recordCount;
        BLFClose(&r);
        LoggerLog("CreateSeedFile: seed already exists (%llu board(s)), skipping: %s\n",
                  count, seedPath);
        return;
    }

    PBOARD pRoot = BoardAllocateFirstBoard();
    if (!pRoot)
        Fatal(FATAL_ALLOCATION_FAILED, "CreateSeedFile: BoardAllocateFirstBoard failed");

    BOARD_KEY key = {};
    key.ullCellsInUse = pRoot->ullCellsInUse;
    key.ullCellColors = pRoot->ullCellColors;
    key.usBoardInfo   = pRoot->usBoardInfo;
    MemFree(pRoot);

    BLFWrite(seedPath, &key, 1);

    LoggerLog("CreateSeedFile: wrote level-0 seed -> '%s' (1 board)\n", seedPath);
}
