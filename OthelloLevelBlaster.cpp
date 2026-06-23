#include "InitLogger.h"
#include "InitSolver.h"
#include "CreateSeedFile.h"
#include "DriveLedger.h"
#include "LevelSolverThread.h"
#include "MergeFiles.h"
#include "StatsListener.h"
#include "BlasterFileName.h"
#include <windows.h>
#include <ctype.h>

OthelloLevelBlasterConfig g_config      = {};
OthelloLevelBlasterState  g_state       = {};
MachineInfo               g_machineInfo = {};

// ============================================================================
// Command-line argument parsing
// ============================================================================

static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --board-size N    Board size (e.g. 4 for 4x4, 6 for 6x6)  [default: 6]\n");
    printf("  --drives LETTERS  Drive letters to use, e.g. DEFY           [default: DEFY]\n");
    printf("  --store-drive L   Drive letter for NAS/store output          [default: Y]\n");
    printf("  --store-dir PATH  Sub-path on store drive (no drive letter)  [default: \\OthelloLevelBlaster\\Store]\n");
    printf("  --cache-dir PATH  Full path for logs and drive-bench cache   [default: C:\\OthelloLevelBlaster\\Cache]\n");
    printf("  --port N          Stats listener TCP port                    [default: 17432]\n");
    printf("  --compress        Compress all files as .blfz (delta+varint, ~7x smaller) [default]\n");
    printf("  --compress-store-only  Compress only store (Y:) output; MW/imerge stay .blf\n");
    printf("  --no-compress     Write all files as .blf (uncompressed)\n");
    printf("  --help            Show this help\n\n");
    printf("Auto-resume: if storeDir already contains level files from a previous run,\n");
    printf("  the solver automatically resumes from the first missing level.\n");
    printf("  To start fresh, delete or move the storeDir manually.\n\n");
}

static void ParseArgs(int argc, char* argv[])
{
    // Defaults
    g_config.boardSize    = 6;
    g_config.storeDrive   = 'Y';
    g_config.statsPort    = 17432;
    g_config.compressMode = COMPRESS_ALL;
    strncpy(g_config.useDrives,           "DEFY",                           sizeof(g_config.useDrives)           - 1);
    strncpy(g_config.cacheDirName,        "C:\\OthelloLevelBlaster\\Cache",  sizeof(g_config.cacheDirName)        - 1);
    strncpy(g_config.storeDirNameNoDrive, "\\OthelloLevelBlaster\\Store",    sizeof(g_config.storeDirNameNoDrive) - 1);

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            exit(0);
        }

#define REQUIRE_NEXT(flag) \
        if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); exit(1); }

        if (strcmp(argv[i], "--compress") == 0)
        {
            g_config.compressMode = COMPRESS_ALL;
        }
        else if (strcmp(argv[i], "--compress-store-only") == 0)
        {
            g_config.compressMode = COMPRESS_STORE_ONLY;
        }
        else if (strcmp(argv[i], "--no-compress") == 0)
        {
            g_config.compressMode = COMPRESS_NONE;
        }
        else if (strcmp(argv[i], "--board-size") == 0)
        {
            REQUIRE_NEXT("--board-size")
            int n = atoi(argv[i]);
            if (n < 2 || n > 12) { printf("ERROR: --board-size must be 2..12\n"); exit(1); }
            g_config.boardSize = (uint8_t)n;
        }
        else if (strcmp(argv[i], "--drives") == 0)
        {
            REQUIRE_NEXT("--drives")
            strncpy(g_config.useDrives, argv[i], sizeof(g_config.useDrives) - 1);
        }
        else if (strcmp(argv[i], "--store-drive") == 0)
        {
            REQUIRE_NEXT("--store-drive")
            g_config.storeDrive = (char)toupper((unsigned char)argv[i][0]);
        }
        else if (strcmp(argv[i], "--store-dir") == 0)
        {
            REQUIRE_NEXT("--store-dir")
            strncpy(g_config.storeDirNameNoDrive, argv[i], sizeof(g_config.storeDirNameNoDrive) - 1);
        }
        else if (strcmp(argv[i], "--cache-dir") == 0)
        {
            REQUIRE_NEXT("--cache-dir")
            strncpy(g_config.cacheDirName, argv[i], sizeof(g_config.cacheDirName) - 1);
        }
        else if (strcmp(argv[i], "--port") == 0)
        {
            REQUIRE_NEXT("--port")
            g_config.statsPort = (uint16_t)atoi(argv[i]);
        }
        else
        {
            printf("ERROR: unknown argument '%s'\n\n", argv[i]);
            PrintUsage(argv[0]);
            exit(1);
        }

#undef REQUIRE_NEXT
    }
}

static void WriteSentinelStats(const char* path, const LevelStats* ls)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t magic = SENTINEL_STATS_MAGIC;
    DWORD nw;
    WriteFile(h, &magic, (DWORD)sizeof(magic), &nw, NULL);
    WriteFile(h, ls,     (DWORD)sizeof(*ls),   &nw, NULL);
    CloseHandle(h);
}

static BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT)
    {
        LoggerLog("Ctrl+C received - requesting graceful shutdown...\n");
        g_state.terminateThreads = true;
        return TRUE;
    }
    return FALSE;
}

static void WaitForPoolIdle(ThreadPool* pPool)
{
    while (pPool->IsBusy())
        Sleep(1);
}

// ============================================================================
// Per-level summary line
//
// Columns (matching OLE layout where possible):
//   Lv  BoardsIn  NewBoards  Pass  GpuDups  MrgDups  UniqueOut  Ends  Fls
//   SlvGB  MrgGB  SlvTm  MrgTm  TotTm  ns/brd  DateTime
// ============================================================================

static void PrintLevelStatsHeader()
{
    LoggerLog(
        "\n  Lv        BoardsIn       NewBoards         Pass         GpuDups"
        "         MrgDups       UniqueOut      Ends  MaxMv    Fls      SlvGB"
        "      MrgGB    SlvTm(s)    MrgTm(s)    TotTm(s)       SlvNs/b"
        "       MrgNs/b       TotNs/b  DateTime\n"
        "  --  --------------  --------------  ----------  --------------"
        "  --------------  --------------  --------  -----  -----  ---------"
        "  ---------  ----------  ----------  ----------  ------------"
        "  ------------  ------------  -------------------\n"
    );
}

static void LogLevelSummary(int level, PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt = pCtx->pState;
    const LevelStats*         ls  = &pSt->levelStats[level];

    uint64_t uniqueOut = (ls->boardsWrittenToDisk >= ls->mrgDupsRemoved)
                         ? ls->boardsWrittenToDisk - ls->mrgDupsRemoved : 0;
    double   slvTm     = ls->solverNanos / 1.0e9;
    double   mrgTm     = (ls->totalNanos - ls->solverNanos) / 1.0e9;
    double   totTm     = ls->totalNanos  / 1.0e9;
    uint64_t brdCount  = ls->boardsReadFromStore > 0 ? ls->boardsReadFromStore : 1;
    uint64_t slvNsBrd  = (uint64_t)(ls->solverNanos / (int64_t)brdCount);
    uint64_t mrgNsBrd  = (uint64_t)((ls->totalNanos - ls->solverNanos) / (int64_t)brdCount);
    uint64_t totNsBrd  = (uint64_t)(ls->totalNanos  / (int64_t)brdCount);
    double   slvGB     = ls->mwBytes         / (1024.0 * 1024.0 * 1024.0);
    double   mrgGB     = ls->mergeActualBytes / (1024.0 * 1024.0 * 1024.0);
    double   mrgEquivGB = ls->mergeBytes      / (1024.0 * 1024.0 * 1024.0);

    LoggerLog(
        "  %2d  %14llu  %14llu  %10llu  %14llu  %14llu  %14llu"
        "  %8llu  %5u  %5llu  %9.2f  %9.2f  %10.3f  %10.3f  %10.3f"
        "  %12llu  %12llu  %12llu  %s\n",
        level,
        (unsigned long long)(ls->boardsReadFromStore + ls->passBoards),
        (unsigned long long)ls->boardsGenerated,
        (unsigned long long)ls->passBoards,
        (unsigned long long)ls->gpuDupsRemoved,
        (unsigned long long)ls->mrgDupsRemoved,
        (unsigned long long)uniqueOut,
        (unsigned long long)ls->terminalBoards,
        ls->maxMovesInLevel,
        (unsigned long long)ls->gpuFlushes,
        slvGB, mrgGB,
        slvTm, mrgTm, totTm,
        (unsigned long long)slvNsBrd,
        (unsigned long long)mrgNsBrd,
        (unsigned long long)totNsBrd,
        ls->completedAt
    );

    // Per-drive breakdown from snapshot captured at level completion
    for (int i = 0; i < ls->numDriveSnapshot; i++)
    {
        const WriterDriveStats* d = &ls->driveSnapshot[i];
        if (d->levelBytesUncompressed > 0
            && d->levelBytesUncompressed != d->levelBytesWritten)
            LoggerLog("      %c:  files=%4llu  %8.2f GB on disk  (%8.2f GB uncomp)  free=%9.2f GB\n",
                      d->driveLetter,
                      (unsigned long long)d->levelFilesWritten,
                      d->levelBytesWritten      / (1024.0 * 1024.0 * 1024.0),
                      d->levelBytesUncompressed / (1024.0 * 1024.0 * 1024.0),
                      d->lastFreeBytes          / (1024.0 * 1024.0 * 1024.0));
        else
            LoggerLog("      %c:  files=%4llu  %8.2f GB  free=%9.2f GB\n",
                      d->driveLetter,
                      (unsigned long long)d->levelFilesWritten,
                      d->levelBytesWritten / (1024.0 * 1024.0 * 1024.0),
                      d->lastFreeBytes     / (1024.0 * 1024.0 * 1024.0));
    }
    if (ls->mergeActualBytes > 0 && ls->mergeActualBytes != ls->mergeBytes)
        LoggerLog("      %c:  files=%4u  %8.2f GB on disk  (%8.2f GB uncomp)  free=%9.2f GB\n",
                  pCtx->pConfig->storeDrive,
                  ls->mergeFilesWritten,
                  mrgGB, mrgEquivGB,
                  ls->storeFreeBytes / (1024.0 * 1024.0 * 1024.0));
    else
        LoggerLog("      %c:  files=%4u  %8.2f GB  free=%9.2f GB\n",
                  pCtx->pConfig->storeDrive,
                  ls->mergeFilesWritten,
                  mrgGB,
                  ls->storeFreeBytes / (1024.0 * 1024.0 * 1024.0));
    LoggerLog("\n");
}

int main(int argc, char* argv[])
{
    ParseArgs(argc, argv);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    InitLogger(&g_config, &g_state);
    InitSolver(&g_config, &g_state, &g_machineInfo);
    CreateSeedFile(&g_config, &g_state);

    SolveContext ctx = { &g_config, &g_state, &g_machineInfo };
    SubmitStatsListenerJob(&ctx);

    // 4 pieces are pre-placed at game start; each level adds one piece.
    // The last level (all squares filled) generates no children but counts terminal boards.
    const int maxLevel   = g_config.boardSize * g_config.boardSize - 3;
    const int startLevel = g_state.resumeLevel;

    if (startLevel > 0)
        LoggerLog("Resuming from level %d (levels 0..%d already complete).\n",
                  startLevel, startLevel - 1);

    PrintLevelStatsHeader();

    // Print restored history for levels already completed before this run.
    // Stats are loaded from the _complete sentinel files by ScanForResumeLevel.
    // Levels whose sentinels were zero-byte (legacy / manual) have totalNanos==0
    // and are silently skipped.
    for (int lvl = 0; lvl < startLevel; lvl++)
        if (g_state.levelStats[lvl].totalNanos > 0)
            LogLevelSummary(lvl, &ctx);

    for (int level = startLevel; level < maxLevel && !g_state.terminateThreads; level++)
    {
        g_state.playLevel = (uint8_t)level;

        // Reset per-level per-thread state
        for (int i = 0; i < g_state.numMergeWriters; i++)
        {
            g_state.mwBlackFileCount[i]     = 0;
            g_state.mwWhiteFileCount[i]     = 0;
            g_state.mwBlackFilesConsumed[i] = 0;
            g_state.mwWhiteFilesConsumed[i] = 0;
        }
        g_state.imergeActive[0] = 0;
        for (int i = 0; i < g_state.numMergeDirs; i++)
        {
            g_state.mergeFileBlackCount[i] = 0;
            g_state.mergeFileWhiteCount[i] = 0;
        }
        g_state.storeMergeBlackFileCount = 0;
        g_state.storeMergeWhiteFileCount = 0;
        g_state.currentLevelTotalBoards  = 0;
        for (int i = 0; i < g_state.numWriterDrives; i++)
        {
            g_state.writerDriveStats[i].levelFilesWritten      = 0;
            g_state.writerDriveStats[i].levelBytesWritten      = 0;
            g_state.writerDriveStats[i].levelBytesUncompressed = 0;
        }
        g_state.levelStats[level] = {};
        ClockStart(&g_state.levelStats[level].startTick);

        // Re-initialize ledgers from the OS at each level start.
        // NVMe and merge drives should be empty (prior level cleaned up);
        // Y: reflects permanent store accumulation.  The 20 GB safety buffer
        // is re-applied so space decisions never consume the last bytes.
        for (int i = 0; i < g_state.numMergeWriters; i++)
            DriveInitLedger(&g_state, g_state.mwDirectory[i][0]);
        for (int i = 0; i < g_state.numMergeDirs; i++)
            DriveInitLedger(&g_state, g_state.mergeDirectory[i][0]);
        DriveInitLedger(&g_state, g_config.storeDrive);

        g_state.currentPhase = "GPU solving";
        SubmitGpuFeederJob(&ctx, (uint8_t)level);
        WaitForPoolIdle(g_state.pGPUFeederThreadPool);

        // Drain any merge-writer jobs still in flight, then flush remaining buffer segments
        WaitForPoolIdle(g_state.pMergeWriterPool);
        if (!g_state.terminateThreads)
        {
            g_state.currentPhase = "Flushing buffers";
            FlushAllMergeWriterBuffers(&ctx);
        }
        g_state.levelStats[level].solverNanos =
            ClockNanosSinceStart(&g_state.levelStats[level].startTick);

        if (!g_state.terminateThreads)
        {
            // Consolidate all NVMe writer files + HDD merge files → single store file on Y:
            g_state.currentPhase = "Merging to store";
            DoEndOfLevelMerge(&ctx);
            g_state.levelStats[level].totalNanos =
                ClockNanosSinceStart(&g_state.levelStats[level].startTick);

            g_state.levelStats[level].storeFreeBytes =
                (uint64_t)DriveAvailable(&g_state, g_config.storeDrive);

            // Populate lastFreeBytes from ledger before snapshotting for the history table
            for (int i = 0; i < g_state.numWriterDrives; i++)
                g_state.writerDriveStats[i].lastFreeBytes =
                    (uint64_t)DriveAvailable(&g_state, g_state.writerDriveStats[i].driveLetter);

            // Snapshot per-drive stats before they're reset at the next level's start
            g_state.levelStats[level].numDriveSnapshot = g_state.numWriterDrives;
            for (int i = 0; i < g_state.numWriterDrives; i++)
                g_state.levelStats[level].driveSnapshot[i] = g_state.writerDriveStats[i];

            SYSTEMTIME _st = {};
            GetLocalTime(&_st);
            snprintf(g_state.levelStats[level].completedAt,
                     sizeof(g_state.levelStats[level].completedAt),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     _st.wYear, _st.wMonth, _st.wDay,
                     _st.wHour, _st.wMinute, _st.wSecond);

            // Write _complete sentinel with full stats payload so a future restart
            // can restore the history table without re-solving completed levels.
            char sentComplete[MAX_FULL_PATH_NAME];
            SentinelNameComplete(sentComplete, sizeof(sentComplete),
                                 g_state.storeDirectory, level + 1);
            WriteSentinelStats(sentComplete, &g_state.levelStats[level]);
        }
        else
        {
            g_state.levelStats[level].totalNanos =
                ClockNanosSinceStart(&g_state.levelStats[level].startTick);
        }

        g_state.currentPhase = nullptr;

        LogLevelSummary(level, &ctx);
    }

    // Print completed-level history table
    if (g_state.playLevel > 0)
    {
        LoggerLog("\n--- Completed level history ---\n");
        PrintLevelStatsHeader();
        for (int lvl = startLevel; lvl <= (int)g_state.playLevel; lvl++)
            LogLevelSummary(lvl, &ctx);
    }

    CleanupSolver(&g_state);
    return 0;
}
