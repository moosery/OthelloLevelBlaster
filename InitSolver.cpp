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

    const size_t kMinStoreBuf  = 1ULL * 1024 * 1024 * 1024;   // 1 GB floor
    // GPU-based minimum per thread: must hold at least one worst-case GPU flush.
    const size_t gpuMinBufSize = pMachineInfo->g_gpuInfo.totalGlobalMemBytes * 8 / 10;

    if (availableMemoryToAllocate < gpuMinBufSize + kMinStoreBuf)
        Fatal(FATAL_INSUFFICIENT_MEMORY,
              "Not enough RAM for even one merge-writer buffer (%zu GB) plus store buffer.",
              gpuMinBufSize / (1024 * 1024 * 1024));

    // Count fast drives first so we can maximize the per-thread MW buffer.
    int numFastDrives = 0;
    for (int i = 0; i < pMachineInfo->g_drives.numDrives; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (d->available && d->driveCategory == DRIVE_CAT_FAST
                         && d->driveLetter != pConfig->storeDrive)
            numFastDrives++;
    }

    // Maximize MW buffer size: a larger buffer accumulates more GPU flushes before a
    // disk write, widening the in-memory dedup window and reducing how much data reaches
    // disk — fewer files, less imerge pressure, smaller end-of-level merge.
    // Divide all available RAM (minus kMinStoreBuf) evenly across fast drives.
    // Fall back to the GPU-sized minimum and cap writer count when memory is tight.
    size_t mwBufSize;
    int numWritersToCreate;
    if (numFastDrives > 0 &&
        (availableMemoryToAllocate - kMinStoreBuf) / (size_t)numFastDrives >= gpuMinBufSize)
    {
        numWritersToCreate = numFastDrives;
        mwBufSize = (availableMemoryToAllocate - kMinStoreBuf) / (size_t)numWritersToCreate;
    }
    else
    {
        // RAM-constrained: keep gpuMinBufSize per thread and cap writer count.
        mwBufSize = gpuMinBufSize;
        numWritersToCreate = (int)((availableMemoryToAllocate - kMinStoreBuf) / mwBufSize);
        if (numWritersToCreate > numFastDrives) numWritersToCreate = numFastDrives;
    }

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

    // Merge-writer buffers: one per thread, sized to fill available RAM (see mwBufSize above).
    // Store buffer gets what remains — approximately kMinStoreBuf.
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

// Find and delete a player output file for a level without validating it.
// Used after finding a "merging" sentinel when we want to purge partial output.
static void deletePlayerOutputFile(const char* storeDir, int level, const char* player)
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
        char fullPath[MAX_FULL_PATH_NAME];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", storeDir, fd.cFileName);
        LoggerLog("  Deleting partial output '%s'\n", fullPath);
        DeleteFileA(fullPath);
        break;
    }
}

// Read LevelStats from a _complete sentinel file.  Returns false if the file is
// zero-byte (legacy / manually created) or does not contain valid stats data.
static bool ReadSentinelStats(const char* path, LevelStats* out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t magic = 0;
    DWORD    nr    = 0;
    bool ok = ReadFile(h, &magic, (DWORD)sizeof(magic), &nr, NULL)
              && nr == sizeof(magic)
              && magic == SENTINEL_STATS_MAGIC
              && ReadFile(h, out, (DWORD)sizeof(*out), &nr, NULL)
              && nr == sizeof(*out);
    CloseHandle(h);
    return ok;
}

static int ScanForResumeLevel(POthelloLevelBlasterState pState)
{
    // Sentinel-aware scan.  For each level:
    //
    //   _complete present            → level fully written; continue to next.
    //                                  If sentinel contains stats payload, restore
    //                                  levelStats[level-1] for history display.
    //   _merging present (no _complete) → DoEndOfLevelMerge was interrupted;
    //                                     delete sentinel + any player files;
    //                                     resume from this level.
    //   Neither sentinel, no player files → level is missing; resume from here.
    //   Neither sentinel, corrupt file    → delete all for this level; re-run.
    //   Neither sentinel, valid file(s)   → backwards-compat: treat as complete.
    //                                       (Old data without sentinels — add
    //                                       manually: type nul > Level_NNNN_complete)
    for (int level = 0; level < MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];

        // Fast path: complete sentinel → level done; try to restore stats.
        SentinelNameComplete(sentPath, sizeof(sentPath), pState->storeDirectory, level);
        if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
        {
            if (level > 0)
            {
                LevelStats restored = {};
                if (ReadSentinelStats(sentPath, &restored))
                    pState->levelStats[level - 1] = restored;
            }
            continue;
        }

        // Merging sentinel → interrupted mid-merge; purge partial output and re-run.
        SentinelNameMerging(sentPath, sizeof(sentPath), pState->storeDirectory, level);
        if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
        {
            LoggerLog("ScanForResumeLevel: level %d merge was interrupted; purging partial output\n", level);
            DeleteFileA(sentPath);
            deletePlayerOutputFile(pState->storeDirectory, level, "black");
            deletePlayerOutputFile(pState->storeDirectory, level, "white");
            return level;
        }

        // No sentinels: check for player files.
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
            if (bs == LFS_VALID) { LoggerLog("  Deleting valid level %d black alongside corrupt white\n", level); DeleteFileA(blackPath); }
            if (ws == LFS_VALID) { LoggerLog("  Deleting valid level %d white alongside corrupt black\n", level); DeleteFileA(whitePath); }
            return level;
        }

        // Valid file(s), no sentinel — old pre-sentinel data; treat as complete.
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
    if (pConfig->compressMode == COMPRESS_ALL && pConfig->lz4Drives[0])
        LoggerLog("  LZ4 drives         : %s (varint+LZ4 -> .blfzl)\n", pConfig->lz4Drives);
    else if (pConfig->compressMode == COMPRESS_ALL)
        LoggerLog("  LZ4 drives         : (none)\n");
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
