#pragma once
#include <stdint.h>
#include <string.h>
#include <windows.h>


enum MemoryMode { MM_RECOMMENDED = 0, MM_USE_MAX, MM_SPECIFIED };

typedef struct __MemoryInfo
{
    MemoryMode requestedMode;   // memory mode requested by user
    uint64_t requestedBytes;    // if MM_SPECIFIED, the user-specified memory size
    uint64_t totalPhys;         // total physical RAM
    uint64_t availPhys;         // available (free) physical RAM
    uint64_t budgetedSize;      // budgeted memory size based on mode and free RAM
} MemoryInfo, *PMemoryInfo;

inline void sizeToGBString(uint64_t bytes, char* outStr, size_t outStrSize)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(outStr, outStrSize, "%.2f GB", (double)bytes / (1024ULL * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(outStr, outStrSize, "%.2f MB", (double)bytes / (1024ULL * 1024));
    else if (bytes >= 1024ULL)
        snprintf(outStr, outStrSize, "%.2f KB", (double)bytes / 1024);
    else
        snprintf(outStr, outStrSize, "%llu bytes", bytes);
}

// Parses "34GB", "34G", "12000MB", "12000M", "512KB", "512K", or a plain integer (bytes).
inline uint64_t ParseMemorySize(const char* s)
{
    if (!s || !*s) return 0;
    char* end = nullptr;
    uint64_t n = (uint64_t)strtoull(s, &end, 10);
    if (!end || !*end) return n;
    while (*end == ' ') ++end;
    if (_stricmp(end, "GB") == 0 || _stricmp(end, "G") == 0) return n * 1024ULL * 1024 * 1024;
    if (_stricmp(end, "MB") == 0 || _stricmp(end, "M") == 0) return n * 1024ULL * 1024;
    if (_stricmp(end, "KB") == 0 || _stricmp(end, "K") == 0) return n * 1024ULL;
    return n;
}

// Fraction of free (available) physical RAM to use per memory mode.
// Adjust these compile-time constants to tune memory pressure.
static constexpr double BUDGET_PCT_MAX         = 0.95;  // leave ~5% of free RAM untouched
static constexpr double BUDGET_PCT_RECOMMENDED = 0.90;  // leave ~10% of free RAM untouched

// Returns the total memory budget based on currently available (free) physical RAM.
// All modes are relative to free RAM, so the result adapts to whatever else is running.
inline void CalcMemoryBudget(PMemoryInfo pMemInfo)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    uint64_t budget;
    switch (pMemInfo->requestedMode)
    {
    case MM_USE_MAX:   budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_MAX);         break;
    case MM_SPECIFIED: budget = min(pMemInfo->requestedBytes,
                                   (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_MAX));   break;
    default:           budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_RECOMMENDED); break;
    }

    pMemInfo->budgetedSize = budget;
    pMemInfo->totalPhys = ms.ullTotalPhys;
    pMemInfo->availPhys = ms.ullAvailPhys;
}
