#include "CreateSeedFile.h"
#include "BlasterFile.h"
#include "BlasterFileName.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>

void CreateSeedFile(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState)
{
    // Level 0 seed is always a single black-turn board
    char seedPath[MAX_FULL_PATH_NAME];
    BLFNameStoreFile(seedPath, sizeof(seedPath), pState->storeDirectory,
                     (int)pConfig->boardSize, 0, BLF_PLAYER_BLACK, 0);

    // Already exists and is valid — restart scenario, nothing to do except
    // ensure the complete sentinel is present (idempotent).
    BLFReader* r = BLFOpen(seedPath);
    if (r)
    {
        uint64_t count = BLFTrailer(r)->recordCount;
        BLFClose(&r);
        LoggerLog("CreateSeedFile: seed already exists (%llu board(s)), skipping: %s\n",
                  count, seedPath);
    }
    else
    {
        PBOARD pRoot = BoardAllocateFirstBoard();
        if (!pRoot)
            Fatal(FATAL_ALLOCATION_FAILED, "CreateSeedFile: BoardAllocateFirstBoard failed");

        BOARD_KEY_DISK dk;
        dk.ullCellsInUse = pRoot->ullCellsInUse;
        dk.ullCellColors = pRoot->ullCellColors;
        MemFree(pRoot);

        BLFWrite(seedPath, &dk, 1);
        LoggerLog("CreateSeedFile: wrote level-0 seed -> '%s' (1 board)\n", seedPath);
    }

    // Level 0 has no DoEndOfLevelMerge, so write its complete sentinel here.
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), pState->storeDirectory, 0);
    HANDLE hs = CreateFileA(sentPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
}
