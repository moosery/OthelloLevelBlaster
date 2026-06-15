#include "LevelSolverThread.h"
#include "MergeFiles.h"
#include "BlasterFile.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <string.h>
#include <algorithm>

// ============================================================================
// Merge-writer pool job
//
// thdIdx is stable per thread (0 = D:, 1 = E:, etc.) and used to select the
// thread's private buffer, directory, and segment state.  No synchronisation
// is needed because each thread only ever touches its own slots.
//
// Flow:
//   1. D2H the sorted+deduped GPU flush into the next segment slot in the
//      thread's buffer.
//   2. Signal hDoneEvent so the GPU feeder can reset the accumulator and
//      continue accumulating the next batch immediately.
//   3. Check whether the buffer can fit another worst-case GPU flush.
//      If not, do the in-memory k-way merge to disk and reset the buffer.
// ============================================================================

static void RunMergeWriterJob(uint32_t thdIdx, PSolveContext pCtx, PFlushDescriptor pDesc)
{
    POthelloLevelBlasterState pSt = pCtx->pState;
    const int ti = (int)thdIdx;

    // Pointer to where the next segment starts in this thread's buffer
    BOARD_KEY* pSegStart = (BOARD_KEY*)pSt->pMWBuffer[ti] + pSt->mwBoardsUsed[ti];

    // D2H: read all unique boards from the GPU accumulator into the buffer
    int remaining = pDesc->uniqueCount;
    int written   = 0;
    while (remaining > 0)
    {
        int got = GpuFlushRead(pDesc->pAccum, (size_t)written, pSegStart + written, remaining);
        if (got == 0) break;
        written   += got;
        remaining -= got;
    }

    // GPU feeder can reset the accumulator and start accumulating the next batch
    SetEvent(pDesc->hDoneEvent);

    // Record this segment
    const int si = pSt->mwSegCount[ti];
    pSt->mwSegOffset[ti][si] = pSt->mwBoardsUsed[ti];
    pSt->mwSegSize[ti][si]   = written;
    pSt->mwSegCount[ti]++;
    pSt->mwBoardsUsed[ti]   += (size_t)written;

    pSt->levelStats[pSt->playLevel].boardsReceivedFromGpu += (uint64_t)written;

    // Flush to disk if the buffer can't fit another worst-case GPU flush,
    // or if we've reached the segment count limit
    bool bufferFull = (pSt->mwBoardsUsed[ti] + pSt->gpuAccumCapacity) * sizeof(BOARD_KEY)
                      > pSt->mwBufferSize;
    bool segsMaxed  = pSt->mwSegCount[ti] >= MAX_MW_SEGS;

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

// Called from the main level loop after the merge-writer pool is idle.
// Flushes any remaining accumulated segments for each thread to disk.
void FlushAllMergeWriterBuffers(PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt = pCtx->pState;
    for (int ti = 0; ti < (int)pSt->numMergeWriters; ti++)
    {
        if (pSt->mwSegCount[ti] > 0)
            FlushMergeWriterBuffer(ti, pCtx);
    }
}

// ============================================================================
// Helpers used by the GPU feeder job
// ============================================================================

static int EnumerateLevelFiles(const char* storeDir, uint8_t level,
                               char** pOutPaths, int maxFiles)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\Level_%04d_file_*.bin",
             storeDir, (int)level);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do
    {
        if (count >= maxFiles) break;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", storeDir, fd.cFileName);
        pOutPaths[count] = (char*)MemMalloc("levelFile path", strlen(full) + 1);
        if (!pOutPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateLevelFiles: cannot allocate path");
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

    // Game-logic stats are valid even when uniqueCount == 0 (e.g. all-terminal last level)
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
    pDesc->uniqueCount = uniqueCount;
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
// ============================================================================

static void RunGpuFeederJob(uint32_t /*thdIdx*/, PSolveContext pCtx, uint8_t level)
{
    POthelloLevelBlasterConfig pCfg = pCtx->pConfig;
    POthelloLevelBlasterState  pSt  = pCtx->pState;
    PMachineInfo               pMI  = pCtx->pMachineInfo;

    const int    optBatch    = pMI->g_gpuInfo.optimalBatchSize;
    const int    maxMoves    = GetMaxMovesForBoardSize(pCfg->boardSize);
    const size_t totalGpuMem = pMI->g_gpuInfo.totalGlobalMemBytes;

    BOARD_KEY* pingPong = (BOARD_KEY*)pSt->pPingPongBuffer;
    BOARD_KEY* slots[PING_PONG_SLOTS];
    for (int i = 0; i < PING_PONG_SLOTS; i++)
        slots[i] = pingPong + (size_t)i * (size_t)optBatch;

    GpuAccumulator* pAccum = GpuAccumulatorCreate(optBatch, maxMoves, totalGpuMem);

    static const int kMaxFiles = 65536;
    char** levelFiles = (char**)MemMalloc("levelFiles", (size_t)kMaxFiles * sizeof(char*));
    if (!levelFiles)
        Fatal(FATAL_ALLOCATION_FAILED, "GpuFeederJob: cannot allocate file list");

    int numFiles = EnumerateLevelFiles(pSt->storeDirectory, level, levelFiles, kMaxFiles);

    int slotIdx = 0;

    for (int fi = 0; fi < numFiles && !pSt->terminateThreads; fi++)
    {
        BLFReader* r = BLFOpen(levelFiles[fi]);
        if (!r)
        {
            LoggerLog("GpuFeederJob: WARNING cannot open '%s', skipping\n", levelFiles[fi]);
            MemFree(levelFiles[fi]);
            continue;
        }

        while (!pSt->terminateThreads)
        {
            int got = BLFRead(r, slots[slotIdx], optBatch);
            if (got == 0) break;

            pSt->levelStats[pSt->playLevel].boardsReadFromStore += (uint64_t)got;

            if (!GpuAccumulatorHasRoom(pAccum, got))
                FlushAccumulator(pAccum, pCtx);

            GpuProcessBatch(pAccum, slots[slotIdx], got);
            slotIdx = (slotIdx + 1) % PING_PONG_SLOTS;
        }

        BLFClose(&r);
        MemFree(levelFiles[fi]);
    }

    if (!pSt->terminateThreads)
        FlushAccumulator(pAccum, pCtx);

    MemFree(levelFiles);
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
