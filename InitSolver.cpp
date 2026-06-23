#include "InitSolver.h"
#include "BlasterFile.h"
#include "BlasterFileName.h"
#include "DriveLedger.h"
#include "OthelloBasics.h"
#include "Utility.h"
#include <windows.h>
#include <shellapi.h>

static void createMergeWriterDirectoryName(char driveLetter, const char* pStoreDirNoDrive,
                                           int dirNumber, char* pOutDir)
{
    snprintf(pOutDir, MAX_FULL_PATH_NAME, "%c:%s\\writerDir_%d",
             driveLetter, pStoreDirNoDrive, dirNumber);
}

static void DeleteDirRecursive(const char* dir)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteDirRecursive(full);
        else
        {
            SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
}

static void computeState(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState,
                         PMachineInfo pMachineInfo)
{
    pState->playLevel        = 0;
    pState->numMergeWriters  = 0;
    pState->terminateThreads = false;
    size_t availableMemoryToAllocate = pMachineInfo->g_memInfo.budgetedSize;

    // Four batch-sized slots: reader keeps 3 filled while GPU reads the other
    pState->pingPongBufferSize = (size_t)pMachineInfo->g_gpuInfo.optimalBatchSize
                                 * sizeof(BOARD_KEY_DISK) * 4;

    if (pState->pingPongBufferSize > availableMemoryToAllocate)
        Fatal(FATAL_INSUFFICIENT_MEMORY,
              "Ping-pong buffer (%zu bytes) exceeds budgeted RAM (%zu bytes).",
              pState->pingPongBufferSize, availableMemoryToAllocate);

    availableMemoryToAllocate -= pState->pingPongBufferSize;

    // Each merge-writer buffer must comfortably hold several worst-case GPU flushes.
    // Worst-case GPU flush ≈ 80% of VRAM.  Using the full 80% as the buffer size
    // lets us accumulate 2–3 GPU flushes (each ~4.4 GB) before writing to disk.
    const size_t mwBufSize     = pMachineInfo->g_gpuInfo.totalGlobalMemBytes * 8 / 10;
    const size_t kMinStoreBuf  = 1ULL * 1024 * 1024 * 1024;   // 1 GB floor

    if (availableMemoryToAllocate < mwBufSize + kMinStoreBuf)
        Fatal(FATAL_INSUFFICIENT_MEMORY,
              "Not enough RAM for even one merge-writer buffer (%zu GB) plus store buffer.",
              mwBufSize / (1024 * 1024 * 1024));

    int memCapWriters = (int)((availableMemoryToAllocate - kMinStoreBuf) / mwBufSize);
    int numFastDrives = 0;
    for (int i = 0; i < pMachineInfo->g_drives.numDrives; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (d->available && d->driveCategory == DRIVE_CAT_FAST
                         && d->driveLetter != pConfig->storeDrive)
            numFastDrives++;
    }

    int numWritersToCreate = (numFastDrives < memCapWriters) ? numFastDrives : memCapWriters;
    if (numWritersToCreate < 1)
        Fatal(FATAL_INSUFFICIENT_MEMORY, "No fast drives available for merge-writer threads.");

    for (int i = 0; i < pMachineInfo->g_drives.numDrives
                     && pState->numMergeWriters < numWritersToCreate; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (!d->available) continue;
        if (d->driveCategory != DRIVE_CAT_FAST) continue;
        if (d->driveLetter == pConfig->storeDrive) continue;
        createMergeWriterDirectoryName(d->driveLetter, pConfig->storeDirNameNoDrive, 0,
                                       pState->mwDirectory[pState->numMergeWriters]);
        pState->numMergeWriters++;
    }

    // One merge dir per medium drive (intermediate merge destination for NVMe overflow)
    pState->numMergeDirs = 0;
    for (int i = 0; i < pMachineInfo->g_drives.numDrives; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (!d->available) continue;
        if (d->driveCategory != DRIVE_CAT_MEDIUM) continue;
        if (d->driveLetter == pConfig->storeDrive) continue;
        snprintf(pState->mergeDirectory[pState->numMergeDirs], MAX_FULL_PATH_NAME,
                 "%c:%s\\mergeDir", d->driveLetter, pConfig->storeDirNameNoDrive);
        pState->numMergeDirs++;
    }

    snprintf(pState->storeDirectory, MAX_FULL_PATH_NAME, "%c:%s\\storeDir",
             pConfig->storeDrive, pConfig->storeDirNameNoDrive);
    snprintf(pState->storeMergeDirectory, MAX_FULL_PATH_NAME, "%c:%s\\storeMergeDir",
             pConfig->storeDrive, pConfig->storeDirNameNoDrive);
    pState->storeMergeBlackFileCount = 0;
    pState->storeMergeWhiteFileCount = 0;

    // Build per-drive stats
    pState->numWriterDrives = 0;
    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        char dl = pState->mwDirectory[i][0];
        int  di = -1;
        for (int j = 0; j < pState->numWriterDrives; j++)
            if (pState->writerDriveStats[j].driveLetter == dl) { di = j; break; }
        if (di < 0)
        {
            di = pState->numWriterDrives++;
            pState->writerDriveStats[di] = {};
            pState->writerDriveStats[di].driveLetter = dl;
        }
        pState->writerDriveStats[di].numDirs++;
    }
    for (int i = 0; i < pState->numWriterDrives; i++)
    {
        pState->writerDriveStats[i].threshold =
            DRIVE_SPACE_LOW_BYTES * (uint64_t)pState->writerDriveStats[i].numDirs;
    }

    // GPU accumulator worst-case capacity (boards) — used by merge-writer HasRoom check.
    // Mirrors the formula in GpuAccumulatorCreate: per-slot cost = 57 bytes; expand
    // overhead is batchSize*16 + 8 bytes (d_input + two atomic counters).
    const size_t gpuBudget    = pMachineInfo->g_gpuInfo.totalGlobalMemBytes * 8 / 10;
    const size_t expandBytes  = (size_t)pMachineInfo->g_gpuInfo.optimalBatchSize
                                * sizeof(BOARD_KEY_DISK) + 2 * sizeof(uint32_t);
    pState->gpuAccumCapacity  = (gpuBudget - expandBytes) / 57;

    // Merge-writer buffers: one per thread, fixed at mwBufSize.
    // Store buffer gets whatever RAM remains.
    pState->mwBufferSize  = mwBufSize;
    pState->storeBufferSize =
        ((availableMemoryToAllocate - mwBufSize * (size_t)pState->numMergeWriters) / 1024) * 1024;

    // Segment tracking is implicitly zero-initialized via g_state = {}; verify explicitly.
    for (int i = 0; i < (int)pState->numMergeWriters; i++)
    {
        pState->mwBlackSegCount[i]      = 0;
        pState->mwBlackBoardsUsed[i]    = 0;
        pState->mwWhiteSegCount[i]      = 0;
        pState->mwWhiteBoardsUsed[i]    = 0;
        pState->mwBlackFileCount[i]     = 0;
        pState->mwWhiteFileCount[i]     = 0;
        pState->mwBlackFilesConsumed[i] = 0;
        pState->mwWhiteFilesConsumed[i] = 0;
    }

    double totalAllocGB = (pState->pingPongBufferSize + pState->storeBufferSize
                           + mwBufSize * (size_t)pState->numMergeWriters)
                          / (1024.0 * 1024.0 * 1024.0);
    LoggerLog("Allocating %.1f GB of buffers...\n", totalAllocGB);

    pState->pPingPongBuffer = MemMalloc("pingPongBuffer", pState->pingPongBufferSize);
    if (!pState->pPingPongBuffer)
        Fatal(FATAL_ALLOCATION_FAILED,
              "computeState: cannot allocate ping-pong buffer (%zu bytes)",
              pState->pingPongBufferSize);

    pState->pStoreBuffer = MemMalloc("storeBuffer", pState->storeBufferSize);
    if (!pState->pStoreBuffer)
        Fatal(FATAL_ALLOCATION_FAILED,
              "computeState: cannot allocate store buffer (%zu bytes)",
              pState->storeBufferSize);

    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        pState->pMWBuffer[i] = MemMalloc("mwBuffer", pState->mwBufferSize);
        if (!pState->pMWBuffer[i])
            Fatal(FATAL_ALLOCATION_FAILED,
                  "computeState: cannot allocate merge-writer buffer %d (%zu bytes)",
                  i, pState->mwBufferSize);
    }
    LoggerLog("Allocation complete.\n");
}

enum LevelFileStatus { LFS_VALID, LFS_CORRUPT, LFS_ABSENT };

// Probes for Level_NNNN_*_<player>_0000.blf[z], validates the trailer, and
// returns LFS_VALID / LFS_CORRUPT (file existed but deleted) / LFS_ABSENT.
// outPath (MAX_FULL_PATH_NAME) is set when the file is found (valid or corrupt).
static LevelFileStatus checkLevelFile(const char* storeDir, int level, const char* player,
                                      char* outPath, size_t outPathSize)
{
    static const char* exts[] = { "blf", "blfz" };
    for (int e = 0; e < 2; e++)
    {
        char pattern[MAX_FULL_PATH_NAME];
        snprintf(pattern, sizeof(pattern), "%s\\Level_%04d_*_%s_0000.%s",
                 storeDir, level, player, exts[e]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        snprintf(outPath, outPathSize, "%s\\%s", storeDir, fd.cFileName);
        BLFReader* r = BLFOpen(outPath);
        if (!r)
        {
            LoggerLog("ScanForResumeLevel: corrupt level %d %s file, deleting '%s'\n",
                      level, player, outPath);
            DeleteFileA(outPath);
            return LFS_CORRUPT;
        }
        BLFClose(&r);
        return LFS_VALID;
    }
    return LFS_ABSENT;
}

static int ScanForResumeLevel(POthelloLevelBlasterState pState)
{
    // Check both players independently for each level.
    // Rules:
    //   - Both absent               → level missing; resume from here.
    //   - Either corrupt            → delete both (valid and corrupt); resume from here
    //                                 so the producing iteration rewrites all output.
    //   - One valid, other absent   → treat as legitimately one-sided (that player had
    //                                 zero boards); level is complete.
    //   - Both valid                → level complete.
    //
    // Undetectable edge case: Ctrl+C between two concurrent merge-thread completions
    // (one player's file fully written, other not yet started and thus absent).
    // Without a sentinel file we cannot distinguish this from a genuine one-sided level.
    for (int level = 0; level < MAX_LEVELS; level++)
    {
        char blackPath[MAX_FULL_PATH_NAME] = {};
        char whitePath[MAX_FULL_PATH_NAME] = {};
        LevelFileStatus bs = checkLevelFile(pState->storeDirectory, level, "black",
                                            blackPath, sizeof(blackPath));
        LevelFileStatus ws = checkLevelFile(pState->storeDirectory, level, "white",
                                            whitePath, sizeof(whitePath));

        if (bs == LFS_ABSENT && ws == LFS_ABSENT)
            return level;

        if (bs == LFS_CORRUPT || ws == LFS_CORRUPT)
        {
            // Delete whichever file is still valid so the producing iteration
            // regenerates both from scratch.
            if (bs == LFS_VALID)
            {
                LoggerLog("ScanForResumeLevel: deleting valid level %d black alongside corrupt white\n", level);
                DeleteFileA(blackPath);
            }
            if (ws == LFS_VALID)
            {
                LoggerLog("ScanForResumeLevel: deleting valid level %d white alongside corrupt black\n", level);
                DeleteFileA(whitePath);
            }
            return level;
        }

        // LFS_VALID or LFS_ABSENT for each player — level complete.
    }
    return MAX_LEVELS;
}

static void cleanUpDrives(POthelloLevelBlasterState pState, PMachineInfo pMachineInfo)
{
    LoggerLog("Purging previous run data...\n");

    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        if (GetFileAttributesA(pState->mwDirectory[i]) == INVALID_FILE_ATTRIBUTES) continue;
        LoggerLog("  Deleting merge-writer dir: %s\n", pState->mwDirectory[i]);
        DeleteDirRecursive(pState->mwDirectory[i]);
    }

    for (int i = 0; i < pState->numMergeDirs; i++)
    {
        if (GetFileAttributesA(pState->mergeDirectory[i]) == INVALID_FILE_ATTRIBUTES) continue;
        LoggerLog("  Deleting merge dir: %s\n", pState->mergeDirectory[i]);
        DeleteDirRecursive(pState->mergeDirectory[i]);
    }

    if (GetFileAttributesA(pState->storeMergeDirectory) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("  Deleting store merge dir: %s\n", pState->storeMergeDirectory);
        DeleteDirRecursive(pState->storeMergeDirectory);
    }

    // storeDir is never purged — it holds the permanent level output archive.
    // Only working directories (writerDirs, mergeDir, storeMergeDir) are ephemeral.
    if (pState->resumeLevel > 0)
        LoggerLog("  Resuming from level %d (levels 0..%d already in store).\n",
                  pState->resumeLevel, pState->resumeLevel - 1);
    else
        LoggerLog("  Store dir kept (fresh run or resuming from level 0).\n");

    RefreshDriveFreeSpace(&pMachineInfo->g_drives);
    LoggerLog("Purge complete.\n");
}

static void createDirectories(POthelloLevelBlasterState pState)
{
    for (int i = 0; i < pState->numMergeWriters; i++)
        if (!CreateFullPath(pState->mwDirectory[i]))
            Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create merge-writer directory '%s'",
                  pState->mwDirectory[i]);

    for (int i = 0; i < pState->numMergeDirs; i++)
        if (!CreateFullPath(pState->mergeDirectory[i]))
            Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create merge directory '%s'",
                  pState->mergeDirectory[i]);

    if (!CreateFullPath(pState->storeMergeDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create store merge directory '%s'",
              pState->storeMergeDirectory);

    if (!CreateFullPath(pState->storeDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create store directory '%s'",
              pState->storeDirectory);
}

void InitSolver(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState,
                PMachineInfo pMachineInfo)
{
    _setmaxstdio(4000);   // k-way merge opens up to MAX_MERGE_FANIN files simultaneously
    SetBoardSizeForRun(pConfig->boardSize);

    for (const char* p = pConfig->useDrives; *p; p++)
    {
        char root[4] = { *p, ':', '\\', '\0' };
        if (GetDriveTypeA(root) == DRIVE_REMOTE) continue;
        SHEmptyRecycleBinA(nullptr, root, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    }

    GetMachineInfo(pConfig->cacheDirName, pConfig->useDrives, pMachineInfo);
    computeState(pConfig, pState, pMachineInfo);
    // ScanForResumeLevel returns the index of the first missing store file.
    // Iteration N reads Level_N and writes Level_N+1, so if Level_N+1 is the
    // first missing file we need to re-run iteration N (= firstMissingFile - 1).
    int firstMissingFile = ScanForResumeLevel(pState);
    pState->resumeLevel  = (firstMissingFile > 0) ? firstMissingFile - 1 : 0;
    cleanUpDrives(pState, pMachineInfo);
    createDirectories(pState);

    // Initialize drive space ledgers after cleanup so we start from clean free space.
    // Each ledger is seeded with (OS free bytes - 20 GB safety buffer).
    for (int i = 0; i < pState->numMergeWriters; i++)
        DriveInitLedger(pState, pState->mwDirectory[i][0]);
    for (int i = 0; i < pState->numMergeDirs; i++)
        DriveInitLedger(pState, pState->mergeDirectory[i][0]);
    DriveInitLedger(pState, pConfig->storeDrive);

    int numMWThreads        = pState->numMergeWriters;
    int numStoreThreads     = 1;
    int numGPUFeederThreads = 1;
    int numStatsThreads     = 1;

    InitializeCriticalSection(&pState->imergeCS);

    pState->pMergeWriterPool = new ThreadPool(numMWThreads, "MergeWriterPool");
    if (!pState->pMergeWriterPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create merge-writer thread pool");

    pState->pStoreThreadPool = new ThreadPool(numStoreThreads, "StoreThreadPool");
    if (!pState->pStoreThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create store thread pool");

    pState->pGPUFeederThreadPool = new ThreadPool(numGPUFeederThreads, "GPUFeederThreadPool");
    if (!pState->pGPUFeederThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create GPU feeder thread pool");

    pState->pStatsThreadPool = new ThreadPool(numStatsThreads, "StatsThreadPool");
    if (!pState->pStatsThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create stats thread pool");

    pState->pMergeWriterPool->Start();
    pState->pStoreThreadPool->Start();
    pState->pGPUFeederThreadPool->Start();
    pState->pStatsThreadPool->Start();

    int lastLevel = (int)pConfig->boardSize * (int)pConfig->boardSize - 4;
    LoggerLog("\nSolver configuration:\n");
    LoggerLog("  Board size         : %dx%d  (levels 0..%d)\n",
              pConfig->boardSize, pConfig->boardSize, lastLevel);
    LoggerLog("  MW threads         : %d\n", numMWThreads);
    LoggerLog("  Store threads      : %d\n", numStoreThreads);
    LoggerLog("  GPU threads        : %d\n", numGPUFeederThreads);
    LoggerLog("  Stats port         : %d\n", (int)pConfig->statsPort);
    LoggerLog("  Store format       : %s\n",
              pConfig->compressMode == COMPRESS_ALL        ? "all files .blfz (delta+varint compressed)" :
              pConfig->compressMode == COMPRESS_STORE_ONLY ? "store .blfz, MW/imerge .blf" :
                                                             "all files .blf (uncompressed)");
    LoggerLog("  Ping-pong buf      : %.1f MB\n",
              pState->pingPongBufferSize / (1024.0 * 1024.0));
    LoggerLog("  MW buf             : %.1f GB x %d threads\n",
              pState->mwBufferSize / (1024.0 * 1024.0 * 1024.0), pState->numMergeWriters);
    LoggerLog("  GPU accum capacity : %zu boards\n", pState->gpuAccumCapacity);
    LoggerLog("  Store buf          : %.1f GB\n",
              pState->storeBufferSize / (1024.0 * 1024.0 * 1024.0));
    LoggerLog("  Merge-writer dirs:\n");
    for (int i = 0; i < pState->numMergeWriters; i++)
        LoggerLog("    [%d] %s\n", i, pState->mwDirectory[i]);
    LoggerLog("  Merge dirs:\n");
    for (int i = 0; i < pState->numMergeDirs; i++)
        LoggerLog("    [%d] %s\n", i, pState->mergeDirectory[i]);
    LoggerLog("  Store merge dir    : %s\n", pState->storeMergeDirectory);
    LoggerLog("  Store dir          : %s\n", pState->storeDirectory);
    if (pState->resumeLevel > 0)
        LoggerLog("  ** Resuming from level %d (levels 0..%d already stored)\n",
                  pState->resumeLevel, pState->resumeLevel - 1);
    LoggerLog("\n");
}

void CleanupSolver(POthelloLevelBlasterState pState)
{
    pState->terminateThreads = true;
    pState->pMergeWriterPool->Stop();
    delete pState->pMergeWriterPool;
    pState->pStoreThreadPool->Stop();
    delete pState->pStoreThreadPool;
    pState->pGPUFeederThreadPool->Stop();
    delete pState->pGPUFeederThreadPool;
    pState->terminateStatsListener = true;
    pState->pStatsThreadPool->Stop();
    delete pState->pStatsThreadPool;

    for (int i = 0; i < pState->numMergeWriters; i++)
        MemFree(pState->pMWBuffer[i]);
    MemFree(pState->pStoreBuffer);
    MemFree(pState->pPingPongBuffer);
    DeleteCriticalSection(&pState->imergeCS);
}
