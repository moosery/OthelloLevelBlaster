#include "LevelSolverThread.h"
#include "DriveLedger.h"
#include "MergeFiles.h"
#include "BlasterFile.h"
#include "BlasterFileName.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <string.h>
#include <algorithm>
#include <windows.h>

// ============================================================================
// Merge-writer pool job
//
// Receives a completed GPU flush (sorted+deduped black and white boards in
// d_gather).  D2H copies each player region into the matching end of the
// thread's two-stack MW buffer, signals the GPU feeder so it can reset the
// accumulator, then checks whether the buffer needs to be flushed to disk.
// ============================================================================

static void RunMergeWriterJob(uint32_t thdIdx, PSolveContext pCtx, PFlushDescriptor pDesc)
{
    POthelloLevelBlasterState pSt = pCtx->pState;
    const int   ti         = (int)thdIdx;
    const int   blackCount = pDesc->blackCount;
    const int   whiteCount = pDesc->whiteCount;

    BOARD_KEY_DISK* mwBuf = (BOARD_KEY_DISK*)pSt->pMWBuffer[ti];
    size_t          mwCap = pSt->mwBufferSize / sizeof(BOARD_KEY_DISK);

    // Compute destinations before D2H
    BOARD_KEY_DISK* blackDest = mwBuf + pSt->mwBlackBoardsUsed[ti];

    // White boards grow inward from the end; compute where new run lands
    size_t whiteStart = mwCap - pSt->mwWhiteBoardsUsed[ti] - (size_t)whiteCount;
    BOARD_KEY_DISK* whiteDest = mwBuf + whiteStart;

    // D2H both player regions
    if (blackCount > 0)
        GpuFlushRead(pDesc->pAccum, BLF_PLAYER_BLACK, 0, blackDest, blackCount);
    if (whiteCount > 0)
        GpuFlushRead(pDesc->pAccum, BLF_PLAYER_WHITE, 0, whiteDest, whiteCount);

    // Signal feeder: D2H done, accumulator can be reset
    SetEvent(pDesc->hDoneEvent);

    // Record black segment
    if (blackCount > 0)
    {
        int bs = pSt->mwBlackSegCount[ti]++;
        pSt->mwBlackSegOffset[ti][bs] = pSt->mwBlackBoardsUsed[ti];
        pSt->mwBlackSegSize[ti][bs]   = blackCount;
        pSt->mwBlackBoardsUsed[ti]   += (size_t)blackCount;
    }

    // Record white segment
    if (whiteCount > 0)
    {
        int ws = pSt->mwWhiteSegCount[ti]++;
        pSt->mwWhiteSegOffset[ti][ws] = whiteStart;
        pSt->mwWhiteSegSize[ti][ws]   = whiteCount;
        pSt->mwWhiteBoardsUsed[ti]   += (size_t)whiteCount;
    }

    pSt->levelStats[pSt->playLevel].boardsReceivedFromGpu += (uint64_t)(blackCount + whiteCount);

    // Flush if the buffer can't fit another worst-case GPU accumulator fill,
    // or if we have reached the per-player segment count limit.
    bool bufferFull = (pSt->mwBlackBoardsUsed[ti] + pSt->mwWhiteBoardsUsed[ti]
                       + pSt->gpuAccumCapacity) * sizeof(BOARD_KEY_DISK) > pSt->mwBufferSize;
    bool segsMaxed  = (pSt->mwBlackSegCount[ti] >= MAX_MW_SEGS)
                   || (pSt->mwWhiteSegCount[ti] >= MAX_MW_SEGS);

    if (bufferFull || segsMaxed)
        FlushMergeWriterBuffer(ti, pCtx);
}

void SubmitMergeWriterJob(PSolveContext pCtx, PFlushDescriptor pDesc)
{
    pCtx->pState->pMergeWriterPool->QueueJob(
        [pCtx, pDesc](uint32_t thdIdx)
        {
            RunMergeWriterJob(thdIdx, pCtx, pDesc);
            MemFree(pDesc);
        }
    );
}

// Flush any remaining accumulated segments for every thread.
void FlushAllMergeWriterBuffers(PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt = pCtx->pState;
    for (int ti = 0; ti < (int)pSt->numMergeWriters; ti++)
    {
        if (pSt->mwBlackSegCount[ti] > 0 || pSt->mwWhiteSegCount[ti] > 0)
            FlushMergeWriterBuffer(ti, pCtx);
    }
}

// ============================================================================
// Helpers used by the GPU feeder job
// ============================================================================

// Enumerate BLF store files for a given level and player into pOutPaths[].
// Returns the count found.
static int EnumerateStoreFilesForLevel(const char* storeDir, int boardSize,
                                        int level, int player,
                                        char** pOutPaths, int maxFiles)
{
    char pattern[MAX_FULL_PATH_NAME];
    BLFPatternStoreFiles(pattern, sizeof(pattern), storeDir, boardSize, level, player);

    // Extract directory component from the pattern
    char dir[MAX_FULL_PATH_NAME];
    strncpy_s(dir, sizeof(dir), pattern, _TRUNCATE);
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) return 0;
    *lastSlash = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        // Also probe for compressed store files.
        BLFZPatternStoreFiles(pattern, sizeof(pattern), storeDir, boardSize, level, player);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
    }

    int count = 0;
    do
    {
        if (count >= maxFiles) break;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        pOutPaths[count] = (char*)MemMalloc("levelFilePath", strlen(full) + 1);
        if (!pOutPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateStoreFilesForLevel: cannot allocate path");
        strcpy(pOutPaths[count], full);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

static void FlushAccumulator(GpuAccumulator* pAccum, PSolveContext pCtx)
{
    uint64_t generated = GpuAccumulatorWriteOffset(pAccum);
    pCtx->pState->levelStats[pCtx->pState->playLevel].boardsGenerated += generated;

    int uniqueCount = GpuFlushPrepare(pAccum);

    pCtx->pState->levelStats[pCtx->pState->playLevel].passBoards     += GpuFlushPassBoards(pAccum);
    pCtx->pState->levelStats[pCtx->pState->playLevel].terminalBoards += GpuFlushTermBoards(pAccum);
    uint32_t flushMax = GpuFlushMaxMoves(pAccum);
    if (flushMax > pCtx->pState->levelStats[pCtx->pState->playLevel].maxMovesInLevel)
        pCtx->pState->levelStats[pCtx->pState->playLevel].maxMovesInLevel = flushMax;

    if (uniqueCount == 0)
    {
        GpuFlushReset(pAccum);
        return;
    }
    pCtx->pState->levelStats[pCtx->pState->playLevel].gpuDupsRemoved +=
        generated - (uint64_t)uniqueCount;
    pCtx->pState->levelStats[pCtx->pState->playLevel].gpuFlushes++;

    PFlushDescriptor pDesc = (PFlushDescriptor)MemMalloc("FlushDescriptor",
                                                          sizeof(FlushDescriptor));
    if (!pDesc)
        Fatal(FATAL_ALLOCATION_FAILED, "FlushAccumulator: cannot allocate FlushDescriptor");

    pDesc->pAccum      = pAccum;
    pDesc->blackCount  = GpuFlushBlackCount(pAccum);
    pDesc->whiteCount  = GpuFlushWhiteCount(pAccum);
    pDesc->hDoneEvent  = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!pDesc->hDoneEvent)
        Fatal(FATAL_ALLOCATION_FAILED, "FlushAccumulator: cannot create done event");

    SubmitMergeWriterJob(pCtx, pDesc);

    WaitForSingleObject(pDesc->hDoneEvent, INFINITE);
    CloseHandle(pDesc->hDoneEvent);

    GpuFlushReset(pAccum);
}

// ============================================================================
// GPU feeder job
//
// Two sub-passes per level: first processes all black-turn input files
// (playerBit = BLF_PLAYER_BLACK), then all white-turn files
// (playerBit = BLF_PLAYER_WHITE).  The MW buffer accumulates boards from
// both players concurrently via its two-stack layout, so no mid-level flush
// between sub-passes is required.
// ============================================================================

static void RunGpuFeederJob(uint32_t /*thdIdx*/, PSolveContext pCtx, uint8_t level)
{
    POthelloLevelBlasterConfig pCfg = pCtx->pConfig;
    POthelloLevelBlasterState  pSt  = pCtx->pState;
    PMachineInfo               pMI  = pCtx->pMachineInfo;

    const int    optBatch    = pMI->g_gpuInfo.optimalBatchSize;
    const int    maxMoves    = GetMaxMovesForBoardSize(pCfg->boardSize);
    const size_t totalGpuMem = pMI->g_gpuInfo.totalGlobalMemBytes;
    const int    boardSize   = (int)pCfg->boardSize;

    BOARD_KEY_DISK* pingPong = (BOARD_KEY_DISK*)pSt->pPingPongBuffer;
    BOARD_KEY_DISK* slots[PING_PONG_SLOTS];
    for (int i = 0; i < PING_PONG_SLOTS; i++)
        slots[i] = pingPong + (size_t)i * (size_t)optBatch;

    GpuAccumulator* pAccum = GpuAccumulatorCreate(optBatch, maxMoves, totalGpuMem);

    static const int kMaxFiles = 65536;
    char** blackFiles = (char**)MemMalloc("blackFiles", (size_t)kMaxFiles * sizeof(char*));
    char** whiteFiles = (char**)MemMalloc("whiteFiles", (size_t)kMaxFiles * sizeof(char*));
    if (!blackFiles || !whiteFiles)
        Fatal(FATAL_ALLOCATION_FAILED, "GpuFeederJob: cannot allocate file lists");

    int numBlack = EnumerateStoreFilesForLevel(pSt->storeDirectory, boardSize, level,
                                               BLF_PLAYER_BLACK, blackFiles, kMaxFiles);
    int numWhite = EnumerateStoreFilesForLevel(pSt->storeDirectory, boardSize, level,
                                               BLF_PLAYER_WHITE, whiteFiles, kMaxFiles);

    // Pre-scan for StatsListener solve-phase % progress
    {
        uint64_t total = 0;
        for (int fi = 0; fi < numBlack; fi++)
        {
            BLFReader* r = BLFOpen(blackFiles[fi]);
            if (r) { total += BLFTrailer(r)->recordCount; BLFClose(&r); }
        }
        for (int fi = 0; fi < numWhite; fi++)
        {
            BLFReader* r = BLFOpen(whiteFiles[fi]);
            if (r) { total += BLFTrailer(r)->recordCount; BLFClose(&r); }
        }
        pSt->currentLevelTotalBoards = total;
    }

    int slotIdx = 0;

    // Sub-pass 1: black-turn input boards -> playerBit = BLF_PLAYER_BLACK
    for (int fi = 0; fi < numBlack && !pSt->terminateThreads; fi++)
    {
        BLFReader* r = BLFOpen(blackFiles[fi]);
        if (!r)
        {
            LoggerLog("GpuFeederJob: WARNING cannot open '%s', skipping\n", blackFiles[fi]);
            MemFree(blackFiles[fi]);
            continue;
        }

        while (!pSt->terminateThreads)
        {
            int got = BLFRead(r, slots[slotIdx], optBatch);
            if (got == 0) break;

            pSt->levelStats[pSt->playLevel].boardsReadFromStore += (uint64_t)got;

            if (!GpuAccumulatorHasRoom(pAccum, got))
                FlushAccumulator(pAccum, pCtx);

            GpuProcessBatch(pAccum, slots[slotIdx], got, BLF_PLAYER_BLACK);
            slotIdx = (slotIdx + 1) % PING_PONG_SLOTS;
        }

        BLFClose(&r);
        MemFree(blackFiles[fi]);
    }

    // Sub-pass 2: white-turn input boards -> playerBit = BLF_PLAYER_WHITE
    for (int fi = 0; fi < numWhite && !pSt->terminateThreads; fi++)
    {
        BLFReader* r = BLFOpen(whiteFiles[fi]);
        if (!r)
        {
            LoggerLog("GpuFeederJob: WARNING cannot open '%s', skipping\n", whiteFiles[fi]);
            MemFree(whiteFiles[fi]);
            continue;
        }

        while (!pSt->terminateThreads)
        {
            int got = BLFRead(r, slots[slotIdx], optBatch);
            if (got == 0) break;

            pSt->levelStats[pSt->playLevel].boardsReadFromStore += (uint64_t)got;

            if (!GpuAccumulatorHasRoom(pAccum, got))
                FlushAccumulator(pAccum, pCtx);

            GpuProcessBatch(pAccum, slots[slotIdx], got, BLF_PLAYER_WHITE);
            slotIdx = (slotIdx + 1) % PING_PONG_SLOTS;
        }

        BLFClose(&r);
        MemFree(whiteFiles[fi]);
    }

    if (!pSt->terminateThreads)
        FlushAccumulator(pAccum, pCtx);

    MemFree(blackFiles);
    MemFree(whiteFiles);
    GpuAccumulatorDestroy(pAccum);
}

void SubmitGpuFeederJob(PSolveContext pCtx, uint8_t level)
{
    pCtx->pState->pGPUFeederThreadPool->QueueJob(
        [pCtx, level](uint32_t thdIdx)
        {
            RunGpuFeederJob(thdIdx, pCtx, level);
        }
    );
}
