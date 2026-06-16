#pragma once
#include "Utility.h"

#define VERSION           "0.1.4"
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
    uint64_t levelBytesWritten;
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
    uint64_t mergeBytes;

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
} OthelloLevelBlasterConfig, *POthelloLevelBlasterConfig;

typedef struct __OthelloLevelBlasterState
{
    uint8_t     playLevel;
    int         resumeLevel;          // first level not found in storeDir at startup (0 = fresh run)
    bool        terminateThreads;
    bool        terminateStatsListener;
    const char* currentPhase;       // points to a string literal; set by main thread at each phase transition
    uint64_t    mergeProgressBytes;    // bytes written to final merge output so far (main thread writes, stats thread reads)
    uint64_t    mergeTotalInputBytes;  // total input bytes for the current end-of-level merge

    // Merge-writer threads: one per NVMe drive, stable thdIdx maps to buffer/dir
    uint8_t numMergeWriters;
    char    mwDirectory[MAX_WRITERS][MAX_FULL_PATH_NAME];
    size_t  mwBufferSize;                           // bytes per merge-writer buffer
    void*   pMWBuffer[MAX_WRITERS];                 // one large buffer per thread
    int     mwFileCount[MAX_WRITERS];               // next output file number (reset each level)
    size_t  gpuAccumCapacity;                       // GPU accumulator board capacity (for HasRoom check)

    // Per-thread segment tracking (no sync needed — each thread owns its own slots)
    int    mwSegCount[MAX_WRITERS];
    size_t mwSegOffset[MAX_WRITERS][MAX_MW_SEGS];  // board offset of each segment in pMWBuffer
    int    mwSegSize[MAX_WRITERS][MAX_MW_SEGS];    // board count of each segment
    size_t mwBoardsUsed[MAX_WRITERS];              // total boards accumulated so far

    // Intermediate merge destinations (medium drives: F:, etc.)
    char    mergeDirectory[MAX_WRITER_DRIVES][MAX_FULL_PATH_NAME];
    uint8_t numMergeDirs;
    int     mergeFileCount[MAX_WRITER_DRIVES];  // access via InterlockedExchangeAdd

    // Fallback intermediate merge destination on the store drive (used when no
    // medium drive has enough space for even one MAX_MERGE_FANIN batch).
    char    storeMergeDirectory[MAX_FULL_PATH_NAME];
    int     storeMergeFileCount;                // access via InterlockedExchangeAdd

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
