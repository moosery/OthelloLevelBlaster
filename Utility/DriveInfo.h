#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DRIVE_SAFETY_MARGIN_BYTES  (200ULL * 1024 * 1024 * 1024)   // 200 GB
#define MAX_SYSTEM_DRIVES 16

// Per-dir write MB/s thresholds for drive categorization (based on benchmarked writeMBs)
#define FAST_DRIVE_MB_THRESHOLD   500.0   // NVMe-class: writer threads target these
#define MEDIUM_DRIVE_MB_THRESHOLD  50.0   // HDD / fast NAS: intermediate merge destinations
                                          // Below MEDIUM threshold: store-only (slow NAS etc.)

typedef enum {
    DRIVE_CAT_FAST   = 0,   // >= 500 MB/s  — writer targets
    DRIVE_CAT_MEDIUM = 1,   // >=  50 MB/s  — intermediate merge destinations
    DRIVE_CAT_SLOW   = 2,   // <   50 MB/s  — store-only
} DriveCategory;

typedef struct __DriveInformation {
    // --- Detection ---
    char     driveLetter;
    bool     available;         // false if drive not accessible or query failed
    bool     isNvme;
    bool     isRotational;      // true = HDD (seeks incur a penalty)
    bool     isNas;             // true = network/remote drive (DRIVE_REMOTE)
    int      primaryDiskNum;    // physical disk number; -1 if unknown
    int      numSpindles;
    uint64_t totalBytes;
    uint64_t freeBytes;
    uint64_t usableBytes;       // freeBytes - DRIVE_SAFETY_MARGIN_BYTES (0 if insufficient)
    char     serial[64];

    // --- Benchmark ---
    bool          benchmarkValid;
    int           optimalDirs;       // concurrency level that maximised combined write throughput
    double        writeMBs;          // per-dir write MB/s at optimalDirs concurrency
    double        readMBs;           // per-dir read MB/s at optimalDirs concurrency
    double        combinedWriteMBs;
    double        combinedReadMBs;
    char          timestamp[32];     // "YYYY-MM-DD HH:MM:SS" when benchmark was last run

    // --- Categorization (derived from writeMBs after benchmark/cache load) ---
    DriveCategory driveCategory;
} DriveInformation, * PDriveInformation;

typedef struct __MachineDriveInfo
{
    DriveInformation drives[MAX_SYSTEM_DRIVES];
    int              numDrives;
} MachineDriveInfo, * PMachineDriveInfo;

// Query and optionally benchmark an array of drives.
//   driveLetters  : null-terminated string of drive letters (e.g. "CDZ").
//                   Pass NULL to auto-enumerate all fixed local drives.
//   pCacheDir     : directory for the driveinfo.json benchmark cache.
//                   Pass NULL to disable caching.
//   forceBenchmark: when true, always re-run the benchmark and overwrite the cache.
void GetDriveInformation(
    PMachineDriveInfo pMachineDriveInfo,
    const char* pCacheDir,
    const char* driveLetters,
    bool        forceBenchmark = false);

// Re-query only freeBytes / usableBytes / totalBytes for each drive.
// Call this after any operation that frees space (recycle bin flush, directory delete)
// to keep the stored numbers accurate.
void RefreshDriveFreeSpace(PMachineDriveInfo pMDI);

// Print a summary (detection + benchmark) for all drives.
void PrintDriveInformation(const PMachineDriveInfo pDrive);
