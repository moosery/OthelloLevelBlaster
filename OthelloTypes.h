#pragma once
#include "Utility.h"

#define VERSION           "0.2.9"
// Compression mode for BLF output files.
#define COMPRESS_NONE       0   // all files uncompressed (.blf)
#define COMPRESS_STORE_ONLY 1   // only store (Y:) output compressed (.blfz); MW/imerge stay .blf
#define COMPRESS_ALL        2   // all files compressed (.blfz)

#define MAX_WRITERS       30
#define MAX_WRITER_DRIVES 26    // at most one entry per drive letter
#define MAX_LEVELS        256   // covers up to 16x16 board (252 levels)
#define MAX_MW_SEGS       8     // max GPU flush segments per merge-writer buffer

// Max simultaneous input files in a single k-way merge pass.
// 256 covers a full NVMe at worst case (3.63 TB / 16 GB flush = ~226 files).
#define MAX_MERGE_FANIN 256

// Drive space threshold — when free bytes on a drive drops below
// DRIVE_SPACE_LOW_BYTES * numDirsOnDrive, trigger a merge-to-store flush.
#define DRIVE_SPACE_LOW_GB    20ULL
#define DRIVE_SPACE_LOW_BYTES (DRIVE_SPACE_LOW_GB * 1024ULL * 1024ULL * 1024ULL)

typedef struct __WriterDriveStats
{
    char     driveLetter;
    int      numDirs;
    uint64_t threshold;
    uint64_t lastFreeBytes;
    uint64_t levelFilesWritten;
    uint64_t levelBytesWritten;       // actual bytes on disk (compressed when COMPRESS_ALL)
    uint64_t levelBytesUncompressed;  // uncompressed equivalent (count * 16 + trailers)
} WriterDriveStats, *PWriterDriveStats;

typedef struct __LevelStats
{
    // Input
    uint64_t boardsReadFromStore;

    // GPU expansion + dedup
    uint64_t boardsGenerated;
    uint64_t gpuDupsRemoved;
    uint64_t gpuFlushes;

    // Merge-writer output
    uint64_t boardsReceivedFromGpu;
    uint64_t boardsWrittenToDisk;
    uint64_t mwFilesCreated;
    uint64_t mwBytes;

    // Merge phase (populated after merge; 0 until then)
    uint64_t mrgDupsRemoved;
    uint64_t mergeBytes;       // uncompressed equivalent (uniqueOut * 16 + trailers)
    uint64_t mergeActualBytes; // actual bytes written to store drive (compressed if .blfz)

    // Game logic
    uint64_t passBoards;
    uint64_t terminalBoards;
    uint32_t maxMovesInLevel;

    // Timing
    ClockTick startTick;
    int64_t   solverNanos;
    int64_t   totalNanos;
    char      completedAt[24];   // "YYYY-MM-DD HH:MM:SS" stamped when level finishes

    // Per-drive snapshot captured at level completion (drives reset each level,
    // so history table reads this instead of the live writerDriveStats)
    WriterDriveStats driveSnapshot[MAX_WRITER_DRIVES];
    int              numDriveSnapshot;
    uint64_t         storeFreeBytes;   // free space on store drive at level completion
} LevelStats, *PLevelStats;

typedef struct __OthelloLevelBlasterConfig
{
    uint8_t  boardSize;
    char     useDrives[64];
    char     cacheDirName[MAX_FULL_PATH_NAME];
    char     storeDirNameNoDrive[MAX_FULL_PATH_NAME];
    char     storeDrive;
    uint16_t statsPort;
    uint8_t  compressMode;         // COMPRESS_NONE / COMPRESS_STORE_ONLY / COMPRESS_ALL
} OthelloLevelBlasterConfig, *POthelloLevelBlasterConfig;

typedef struct __OthelloLevelBlasterState
{
    uint8_t     playLevel;
    int         resumeLevel;          // first level not found in storeDir at startup (0 = fresh run)
    bool        terminateThreads;
    bool        terminateStatsListener;
    const char* currentPhase;       // points to a string literal; set by main thread at each phase transition
    volatile int64_t   mergeProgressBytes;   // bytes written to final merge output (two merge threads write; stats thread reads)
    uint64_t           mergeTotalInputBytes; // total input bytes for current end-of-level merge (set before threads start)

    // Per-player cascade progress — populated when CascadingMerge triggers during DoEndOfLevelMerge.
    // Indexed by BLF_PLAYER_WHITE(0) / BLF_PLAYER_BLACK(1).
    // Written by the merge thread, read by the stats thread (no lock needed; display-only).
    bool             cascadeActive[2];              // true while intermediate groups are running
    int              cascadeNumGroups[2];            // total intermediate groups in this cascade
    int              cascadeGroupsDone[2];           // groups fully written to temp so far
    volatile int64_t cascadeGroupProgressBytes[2];  // bytes written to current group's temp file
    uint64_t    currentLevelTotalBoards; // total boards in current level's input file(s); set by GPU feeder before reading starts

    // Merge-writer threads: one per NVMe drive, stable thdIdx maps to buffer/dir
    uint8_t numMergeWriters;
    char    mwDirectory[MAX_WRITERS][MAX_FULL_PATH_NAME];
    size_t  mwBufferSize;                           // bytes per merge-writer buffer
    void*   pMWBuffer[MAX_WRITERS];                 // one large buffer per thread
    int     mwBlackFileCount[MAX_WRITERS];           // next black-turn output file number (reset each level)
    int     mwWhiteFileCount[MAX_WRITERS];           // next white-turn output file number (reset each level)
    size_t  gpuAccumCapacity;                       // GPU accumulator board capacity (for HasRoom check)

    // Per-thread two-stack segment tracking — black grows from top, white from bottom.
    // No sync needed: each thread only touches its own slots.
    int    mwBlackSegCount[MAX_WRITERS];
    size_t mwBlackSegOffset[MAX_WRITERS][MAX_MW_SEGS];
    int    mwBlackSegSize[MAX_WRITERS][MAX_MW_SEGS];
    size_t mwBlackBoardsUsed[MAX_WRITERS];
    int    mwWhiteSegCount[MAX_WRITERS];
    size_t mwWhiteSegOffset[MAX_WRITERS][MAX_MW_SEGS];
    int    mwWhiteSegSize[MAX_WRITERS][MAX_MW_SEGS];
    size_t mwWhiteBoardsUsed[MAX_WRITERS];

    // Intermediate merge destinations (medium drives: F:, etc.)
    char    mergeDirectory[MAX_WRITER_DRIVES][MAX_FULL_PATH_NAME];
    uint8_t numMergeDirs;
    int     mergeFileBlackCount[MAX_WRITER_DRIVES];  // access via InterlockedExchangeAdd
    int     mergeFileWhiteCount[MAX_WRITER_DRIVES];  // access via InterlockedExchangeAdd

    // Per-drive space ledger (indexed by driveLetter - 'A').
    // Initialized from the OS after cleanup; updated atomically on every write and delete.
    // A 20 GB safety buffer is subtracted at init so reservations never reach the last
    // bytes on a drive.  Replaces all ad-hoc GetDiskFreeSpaceExA calls at decision points.
    volatile int64_t driveLedger[26];

    // Per-writer intermediate merge progress (written by MW threads, read by stats thread).
    // imergeActive[i] is set to 1 before the merge and 0 after; the other fields are
    // populated before imergeActive is set so the stats reader always sees consistent data.
    int      imergeActive[MAX_WRITERS];
    uint64_t imergeTotalInputBytes[MAX_WRITERS];
    uint64_t imergeDoneInputBytes[MAX_WRITERS];

    // Fallback intermediate merge destination on the store drive (used when no
    // medium drive has enough space for even one MAX_MERGE_FANIN batch).
    char    storeMergeDirectory[MAX_FULL_PATH_NAME];
    int     storeMergeBlackFileCount;           // access via InterlockedExchangeAdd
    int     storeMergeWhiteFileCount;           // access via InterlockedExchangeAdd

    // Store (slow/NAS drive: Y:)
    char    storeDirectory[MAX_FULL_PATH_NAME];
    char    logFileName[MAX_FULL_PATH_NAME];

    // Ping-pong buffer (GPU feeder)
    size_t  pingPongBufferSize;
    void*   pPingPongBuffer;

    // Store buffer (future store thread)
    size_t  storeBufferSize;
    void*   pStoreBuffer;

    // Per-drive stats
    WriterDriveStats writerDriveStats[MAX_WRITER_DRIVES];
    int              numWriterDrives;

    // Per-level stats history
    LevelStats levelStats[MAX_LEVELS];

    ThreadPool* pMergeWriterPool;
    ThreadPool* pStoreThreadPool;
    ThreadPool* pGPUFeederThreadPool;
    ThreadPool* pStatsThreadPool;
} OthelloLevelBlasterState, *POthelloLevelBlasterState;
