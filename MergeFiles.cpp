#include "MergeFiles.h"
#include "BlasterFile.h"
#include "BlasterFileName.h"
#include "DriveLedger.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>
#include <algorithm>
#include <queue>
#include <thread>
#include <vector>

// ============================================================================
// Min-heap entry for file-based k-way merge (16-byte disk keys)
// ============================================================================

struct MergeHead
{
    BOARD_KEY_DISK  key;
    BLFReader*      pReader;
};

struct MergeHeadGreater
{
    bool operator()(const MergeHead& a, const MergeHead& b) const
    {
        if (a.key.ullCellsInUse != b.key.ullCellsInUse)
            return a.key.ullCellsInUse > b.key.ullCellsInUse;
        return a.key.ullCellColors > b.key.ullCellColors;
    }
};

// ============================================================================
// Min-heap entry for in-memory k-way merge (BOARD_KEY_DISK — two-stack layout)
// ============================================================================

struct InMemDiskHead
{
    const BOARD_KEY_DISK* pCurrent;
    const BOARD_KEY_DISK* pEnd;
};

struct InMemDiskHeadGreater
{
    bool operator()(const InMemDiskHead& a, const InMemDiskHead& b) const
    {
        if (a.pCurrent->ullCellsInUse != b.pCurrent->ullCellsInUse)
            return a.pCurrent->ullCellsInUse > b.pCurrent->ullCellsInUse;
        return a.pCurrent->ullCellColors > b.pCurrent->ullCellColors;
    }
};

// ============================================================================
// Helpers
// ============================================================================

// Enumerate all BLF files matching fullPattern (e.g. "D:\dir\writer_black_*.blf").
// Extracts the directory from fullPattern automatically.
static int EnumerateByPattern(const char* fullPattern, char** outPaths, int maxPaths,
                               uint64_t* pTotalBytes, uint64_t* outSizes = nullptr)
{
    char dir[MAX_FULL_PATH_NAME];
    strncpy_s(dir, sizeof(dir), fullPattern, _TRUNCATE);
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) { *pTotalBytes = 0; return 0; }
    *lastSlash = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(fullPattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { *pTotalBytes = 0; return 0; }

    int count    = 0;
    *pTotalBytes = 0;
    do
    {
        if (count >= maxPaths) break;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        outPaths[count] = (char*)MemMalloc("enumPath", strlen(full) + 1);
        if (!outPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateByPattern: cannot allocate path");
        strcpy(outPaths[count], full);
        ULARGE_INTEGER sz;
        sz.LowPart  = fd.nFileSizeLow;
        sz.HighPart = (DWORD)fd.nFileSizeHigh;
        *pTotalBytes += sz.QuadPart;
        if (outSizes) outSizes[count] = sz.QuadPart;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

// File-based k-way merge of player-homogeneous files (all same player).
// Reads/writes BOARD_KEY_DISK (16 bytes).  Deduplicates on (cellsInUse, cellColors).
static uint64_t KWayMergeFiles(char** inputPaths, int numInputs, const char* outputPath,
                                volatile int64_t* pProgressBytes, bool compressed = false,
                                const volatile bool* pTerminate = nullptr)
{
    std::priority_queue<MergeHead, std::vector<MergeHead>, MergeHeadGreater> heap;

    for (int i = 0; i < numInputs; i++)
    {
        BLFReader* r = BLFOpen(inputPaths[i]);
        if (!r)
        {
            LoggerLog("KWayMerge: WARNING skipping unreadable file '%s'\n", inputPaths[i]);
            continue;
        }
        BOARD_KEY_DISK first;
        if (BLFRead(r, &first, 1) == 1)
            heap.push({ first, r });
        else
            BLFClose(&r);
    }

    BLFWriter*     pw      = compressed ? BLFWriterOpenZ(outputPath) : BLFWriterOpen(outputPath);
    BOARD_KEY_DISK lastKey = {};
    bool           hasLast = false;

    while (!heap.empty())
    {
        if (pTerminate && *pTerminate) break;

        MergeHead top = heap.top();
        heap.pop();

        if (pProgressBytes)
            InterlockedAdd64((volatile LONG64*)pProgressBytes, (LONG64)sizeof(BOARD_KEY_DISK));

        bool isDup = hasLast
                     && top.key.ullCellsInUse == lastKey.ullCellsInUse
                     && top.key.ullCellColors == lastKey.ullCellColors;
        if (!isDup)
        {
            BLFWriterRecord(pw, &top.key);
            lastKey = top.key;
            hasLast = true;
        }

        BOARD_KEY_DISK next;
        if (BLFRead(top.pReader, &next, 1) == 1)
        {
            top.key = next;
            heap.push(top);
        }
        else
        {
            BLFClose(&top.pReader);
        }
    }

    // Close any readers still in the heap (handles early termination cleanly).
    while (!heap.empty())
    {
        MergeHead top = heap.top(); heap.pop();
        BLFClose(&top.pReader);
    }

    return BLFWriterClose(pw);
}

// pCtx is non-null only on the outer call; nullptr is passed for the recursive final-pass
// call so cascade tracking is not re-entered.
// tempDirs is an ordered list of candidate directories for cascade temp files
// (fastest/most-available first), with storeMergeDirectory as the last-resort entry.
//
// Group sizing is dynamic: for each group we ask each drive "how many files can you
// hold right now?" and write as many as fit to the best available drive (F: first).
// This fills F: as fully as possible before spilling any files to Y:.  After each
// group's dedup savings are reclaimed, F: may have room again for the next group.
static uint64_t CascadingMerge(char** inputPaths, int numInputs,
                                 const char** tempDirs, int numTempDirs,
                                 const char* finalOutPath,
                                 int* pTempCount, int level, int player,
                                 volatile int64_t* pProgressBytes,
                                 PSolveContext pCtx, bool compressFinal = false,
                                 bool compressIntermediate = false,
                                 const volatile bool* pTerminate = nullptr)
{
    const volatile bool* pTerm = pCtx ? &pCtx->pState->terminateThreads : pTerminate;

    if (numInputs <= MAX_MERGE_FANIN)
        return KWayMergeFiles(inputPaths, numInputs, finalOutPath, pProgressBytes,
                              compressFinal, pTerm);

    POthelloLevelBlasterState pSt = pCtx ? pCtx->pState : nullptr;

    // Upper bound on groups is numInputs (1 file per group in the extreme case).
    // In practice groups are large, but we allocate conservatively.
    char**   tempPaths       = (char**)MemMalloc("cascadeTempPaths",
                                                   (size_t)numInputs * sizeof(char*));
    int64_t* tempActualSizes = pSt
        ? (int64_t*)MemMalloc("cascadeTempSizes", (size_t)numInputs * sizeof(int64_t))
        : nullptr;
    if (!tempPaths || (pSt && !tempActualSizes))
        Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp arrays");

    // Seed the status display with a minimum-group estimate; updated if we create more.
    if (pSt)
    {
        pSt->cascadeNumGroups[player]          = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
        pSt->cascadeGroupsDone[player]         = 0;
        pSt->cascadeGroupProgressBytes[player] = 0;
        pSt->cascadeActive[player]             = true;
    }

    int numTemps = 0;
    int start    = 0;
    while (start < numInputs)
    {
        if (pTerm && *pTerm) break;

        int windowSize = (std::min)(MAX_MERGE_FANIN, numInputs - start);

        // Precompute file sizes for the next window of up to MAX_MERGE_FANIN files.
        // Used by each drive check so GetFileAttributesExA is called once per file.
        int64_t fileSzCache[MAX_MERGE_FANIN] = {};
        for (int k = 0; k < windowSize; k++)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExA(inputPaths[start + k], GetFileExInfoStandard, &fad))
                fileSzCache[k] = ((int64_t)fad.nFileSizeHigh << 32)
                               | (int64_t)fad.nFileSizeLow;
        }

        // For each candidate drive (F: first, Y: last resort): count how many
        // consecutive files from 'start' fit within the drive's available ledger space.
        // Use the first drive that can accept at least one file.
        const char* chosenDir  = (numTempDirs > 0) ? tempDirs[0] : nullptr;
        int         groupSize  = windowSize;
        int64_t     groupBytes = 0;
        for (int k = 0; k < windowSize; k++) groupBytes += fileSzCache[k];

        if (pSt && numTempDirs > 0)
        {
            chosenDir  = nullptr;
            groupSize  = 0;
            groupBytes = 0;
            for (int d = 0; d < numTempDirs; d++)
            {
                int64_t avail = DriveAvailable(pSt, tempDirs[d][0]);
                int64_t accum = 0;
                int     count = 0;
                for (int k = 0; k < windowSize; k++)
                {
                    if (accum + fileSzCache[k] > avail) break;
                    accum += fileSzCache[k];
                    count++;
                }
                if (count == 0) continue;
                if (DriveReserve(pSt, tempDirs[d][0], accum))
                {
                    chosenDir  = tempDirs[d];
                    groupSize  = count;
                    groupBytes = accum;
                    break;
                }
                // DriveReserve failed (concurrent allocation narrowed the window) — try next.
            }
            if (!chosenDir)
                Fatal(FATAL_DRIVE_SPACE,
                      "CascadingMerge: %s group %d — no temp drive has room for even one file",
                      BLFPlayerStr(player), numTemps + 1);

            // Keep the status group-count estimate current if we're creating more groups.
            if (numTemps + 1 > pSt->cascadeNumGroups[player])
                pSt->cascadeNumGroups[player] = numTemps + 1;
        }

        if (pSt) pSt->cascadeGroupProgressBytes[player] = 0;

        LoggerLog("CascadingMerge: %s group %d -> %c: (%d files, %.2f GB input)\n",
                  BLFPlayerStr(player), numTemps + 1, chosenDir[0],
                  groupSize, groupBytes / (1024.0 * 1024.0 * 1024.0));

        char tempPath[MAX_FULL_PATH_NAME];
        if (compressIntermediate)
            BLFZNameCascadeTemp(tempPath, sizeof(tempPath), chosenDir, level, player, (*pTempCount)++);
        else
            BLFNameCascadeTemp(tempPath, sizeof(tempPath), chosenDir, level, player, (*pTempCount)++);

        uint64_t tempUnique = KWayMergeFiles(inputPaths + start, groupSize, tempPath,
                                              pSt ? &pSt->cascadeGroupProgressBytes[player]
                                                  : nullptr,
                                              compressIntermediate, pTerm);

        if (pSt)
        {
            int64_t tempActual;
            if (compressIntermediate)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                tempActual = (int64_t)sizeof(BlasterFileTrailer);
                if (GetFileAttributesExA(tempPath, GetFileExInfoStandard, &fad))
                    tempActual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                tempActual = (int64_t)(tempUnique * sizeof(BOARD_KEY_DISK)
                             + sizeof(BlasterFileTrailer));
            }
            tempActualSizes[numTemps] = tempActual;
            // Reclaim the dedup savings from the per-group reservation immediately.
            DriveReclaim(pSt, chosenDir[0], groupBytes - tempActual);
        }

        tempPaths[numTemps] = (char*)MemMalloc("cascadeTempPath", strlen(tempPath) + 1);
        if (!tempPaths[numTemps])
            Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path");
        strcpy(tempPaths[numTemps], tempPath);
        numTemps++;

        if (pSt) pSt->cascadeGroupsDone[player]++;
        start += groupSize;
    }

    if (pSt) pSt->cascadeActive[player] = false;

    uint64_t unique = CascadingMerge(tempPaths, numTemps, tempDirs, numTempDirs,
                                      finalOutPath, pTempCount, level, player,
                                      pProgressBytes, nullptr, compressFinal,
                                      compressIntermediate, pTerm);

    for (int i = 0; i < numTemps; i++)
    {
        // Use tempPaths[i][0] (drive letter from path) — temps may be on different drives.
        if (pSt) DriveReclaim(pSt, tempPaths[i][0], tempActualSizes[i]);
        DeleteFileA(tempPaths[i]);
        MemFree(tempPaths[i]);
    }
    MemFree(tempPaths);
    if (tempActualSizes) MemFree(tempActualSizes);

    return unique;
}

// Forward declaration (defined after FlushMergeWriterBuffer)
static void DoCrossDriveIntermediateMerge(PSolveContext pCtx);

// ============================================================================
// FlushMergeWriterBuffer
//
// In-memory k-way merge of the two-stack MW buffer for thread ti.
// Black segments occupy the top of the buffer (indices [0..mwBlackBoardsUsed-1]).
// White segments occupy the bottom (indices [cap-mwWhiteBoardsUsed..cap-1]).
// Each player's sorted runs are merged independently and written to separate
// player-tagged BLF files.  Deduplication is per player stream.
// ============================================================================

void FlushMergeWriterBuffer(int ti, PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt  = pCtx->pState;
    int                       level = (int)pSt->playLevel;

    bool hasBlack = pSt->mwBlackSegCount[ti] > 0;
    bool hasWhite = pSt->mwWhiteSegCount[ti] > 0;
    if (!hasBlack && !hasWhite) return;

    bool compressMW = (pCtx->pConfig->compressMode == COMPRESS_ALL);

    BOARD_KEY_DISK* mwBuf = (BOARD_KEY_DISK*)pSt->pMWBuffer[ti];

    uint64_t blackCount = 0, whiteCount = 0;
    int      filesCreated = 0;
    uint64_t fileBytes    = 0;

    // --- Black stream: merge sorted runs from the top of the buffer ---
    if (hasBlack)
    {
        // Use the current count as the file index; increment AFTER close so that
        // at any moment mwBlackFileCount[ti] == number of fully-written files on disk.
        // DoCrossDriveIntermediateMerge relies on this guarantee for safe enumeration.
        int blackFileIdx = pSt->mwBlackFileCount[ti];
        char blackPath[MAX_FULL_PATH_NAME];
        if (compressMW)
            BLFZNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                               BLF_PLAYER_BLACK, blackFileIdx);
        else
            BLFNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                              BLF_PLAYER_BLACK, blackFileIdx);
        BLFWriter* pw = compressMW ? BLFWriterOpenZ(blackPath) : BLFWriterOpen(blackPath);

        std::priority_queue<InMemDiskHead, std::vector<InMemDiskHead>, InMemDiskHeadGreater> heap;
        for (int s = 0; s < pSt->mwBlackSegCount[ti]; s++)
        {
            const BOARD_KEY_DISK* pSeg = mwBuf + pSt->mwBlackSegOffset[ti][s];
            if (pSt->mwBlackSegSize[ti][s] > 0)
                heap.push({ pSeg, pSeg + pSt->mwBlackSegSize[ti][s] });
        }

        BOARD_KEY_DISK lastKey = {}; bool hasLast = false;
        while (!heap.empty() && !pCtx->pState->terminateThreads)
        {
            InMemDiskHead top = heap.top(); heap.pop();
            bool dup = hasLast
                       && top.pCurrent->ullCellsInUse == lastKey.ullCellsInUse
                       && top.pCurrent->ullCellColors == lastKey.ullCellColors;
            if (!dup) { BLFWriterRecord(pw, top.pCurrent); lastKey = *top.pCurrent; hasLast = true; }
            if (++top.pCurrent < top.pEnd) heap.push(top);
        }

        uint64_t blackFileBytes = 0;
        blackCount = BLFWriterClose(pw, &blackFileBytes);
        if (blackCount == 0) { DeleteFileA(blackPath); }
        else { pSt->mwBlackFileCount[ti]++; fileBytes += blackFileBytes; filesCreated++; }
    }

    // --- White stream: merge sorted runs from the bottom of the buffer ---
    if (hasWhite)
    {
        int whiteFileIdx = pSt->mwWhiteFileCount[ti];
        char whitePath[MAX_FULL_PATH_NAME];
        if (compressMW)
            BLFZNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                               BLF_PLAYER_WHITE, whiteFileIdx);
        else
            BLFNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                              BLF_PLAYER_WHITE, whiteFileIdx);
        BLFWriter* pw = compressMW ? BLFWriterOpenZ(whitePath) : BLFWriterOpen(whitePath);

        std::priority_queue<InMemDiskHead, std::vector<InMemDiskHead>, InMemDiskHeadGreater> heap;
        for (int s = 0; s < pSt->mwWhiteSegCount[ti]; s++)
        {
            const BOARD_KEY_DISK* pSeg = mwBuf + pSt->mwWhiteSegOffset[ti][s];
            if (pSt->mwWhiteSegSize[ti][s] > 0)
                heap.push({ pSeg, pSeg + pSt->mwWhiteSegSize[ti][s] });
        }

        BOARD_KEY_DISK lastKey = {}; bool hasLast = false;
        while (!heap.empty() && !pCtx->pState->terminateThreads)
        {
            InMemDiskHead top = heap.top(); heap.pop();
            bool dup = hasLast
                       && top.pCurrent->ullCellsInUse == lastKey.ullCellsInUse
                       && top.pCurrent->ullCellColors == lastKey.ullCellColors;
            if (!dup) { BLFWriterRecord(pw, top.pCurrent); lastKey = *top.pCurrent; hasLast = true; }
            if (++top.pCurrent < top.pEnd) heap.push(top);
        }

        uint64_t whiteFileBytes = 0;
        whiteCount = BLFWriterClose(pw, &whiteFileBytes);
        if (whiteCount == 0) { DeleteFileA(whitePath); }
        else { pSt->mwWhiteFileCount[ti]++; fileBytes += whiteFileBytes; filesCreated++; }
    }

    // Reset segment tracking for this thread
    pSt->mwBlackSegCount[ti]   = 0;
    pSt->mwBlackBoardsUsed[ti] = 0;
    pSt->mwWhiteSegCount[ti]   = 0;
    pSt->mwWhiteBoardsUsed[ti] = 0;

    uint64_t unique = blackCount + whiteCount;
    uint64_t uncompressedBytes = unique * sizeof(BOARD_KEY_DISK)
                               + (uint64_t)filesCreated * sizeof(BlasterFileTrailer);

    pSt->levelStats[level].boardsWrittenToDisk += unique;
    pSt->levelStats[level].mwFilesCreated      += filesCreated;
    pSt->levelStats[level].mwBytes             += fileBytes;

    // Debit the NVMe ledger for bytes just written, then check merge triggers.
    char driveLetter = pSt->mwDirectory[ti][0];
    DriveDebit(pSt, driveLetter, (int64_t)fileBytes);

    bool needsMerge = false;
    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        if (pSt->writerDriveStats[i].driveLetter == driveLetter)
        {
            pSt->writerDriveStats[i].levelFilesWritten      += filesCreated;
            pSt->writerDriveStats[i].levelBytesWritten      += fileBytes;
            pSt->writerDriveStats[i].levelBytesUncompressed += uncompressedBytes;
            if (DriveAvailable(pSt, driveLetter) < (int64_t)pSt->writerDriveStats[i].threshold)
                needsMerge = true;
            break;
        }
    }
    if (!needsMerge)
    {
        // File-count trigger: merge when total unconsumed files per color >= MAX_MERGE_FANIN.
        int totalBlack = 0, totalWhite = 0;
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            totalBlack += pSt->mwBlackFileCount[i] - pSt->mwBlackFilesConsumed[i];
            totalWhite += pSt->mwWhiteFileCount[i] - pSt->mwWhiteFilesConsumed[i];
        }
        if (totalBlack >= MAX_MERGE_FANIN || totalWhite >= MAX_MERGE_FANIN)
            needsMerge = true;
    }
    if (needsMerge)
        DoCrossDriveIntermediateMerge(pCtx);
}

// ============================================================================
// DoCrossDriveIntermediateMerge
//
// Triggered when total unconsumed writer files across ALL NVMe drives (per
// color) reaches MAX_MERGE_FANIN, or when a single drive's free space drops
// below its threshold.
//
// Merges all unconsumed writer files from every MW directory for each player
// (black then white) into a single imerge file on F:.  If F: cannot hold the
// output, performs a TOTAL FLUSH: also pulls in all existing F: imerge files
// for this level and player, merging the combined set to Y: — clearing both
// the NVMe drives and F: in one shot so all fast drives are free again.
//
// Uses TryEnterCriticalSection so the second MW thread skips gracefully when
// the first is already running a merge.  File counts are snapshotted under
// the lock; because counts are incremented AFTER close, all files with index
// < snapshot[i] are guaranteed complete and safe to read/delete.
// ============================================================================

static void DoCrossDriveIntermediateMerge(PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt      = pCtx->pState;
    int                       level    = (int)pSt->playLevel;
    bool                      compress = (pCtx->pConfig->compressMode == COMPRESS_ALL);

    if (!TryEnterCriticalSection(&pSt->imergeCS))
        return;  // another MW thread is already handling this

    // Re-check under the lock: counts may have dropped since the caller checked.
    {
        int bk = 0, wh = 0;
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            bk += pSt->mwBlackFileCount[i] - pSt->mwBlackFilesConsumed[i];
            wh += pSt->mwWhiteFileCount[i] - pSt->mwWhiteFilesConsumed[i];
        }
        bool spaceOk = true;
        for (int i = 0; i < pSt->numMergeWriters && spaceOk; i++)
        {
            char dl = pSt->mwDirectory[i][0];
            for (int j = 0; j < pSt->numWriterDrives; j++)
            {
                if (pSt->writerDriveStats[j].driveLetter == dl
                    && DriveAvailable(pSt, dl) < (int64_t)pSt->writerDriveStats[j].threshold)
                { spaceOk = false; break; }
            }
        }
        if (bk < MAX_MERGE_FANIN && wh < MAX_MERGE_FANIN && spaceOk)
        {
            LeaveCriticalSection(&pSt->imergeCS);
            return;
        }
    }

    // Snapshot completed-file counts.
    // Because counts are incremented AFTER BLFWriterClose, files [consumed..snap)
    // are all fully written and closed — safe to enumerate and delete.
    int snapBlack[MAX_WRITERS] = {}, snapWhite[MAX_WRITERS] = {};
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        snapBlack[i] = pSt->mwBlackFileCount[i];
        snapWhite[i] = pSt->mwWhiteFileCount[i];
    }

    pSt->imergeActive[0]          = 1;
    pSt->imergeTotalInputBytes[0] = 0;
    pSt->imergeDoneInputBytes[0]  = 0;

    // Upper bound: MAX_MERGE_FANIN * numWriters writer files + imerge files on F:
    const int kMaxFiles = MAX_MERGE_FANIN * MAX_WRITERS + 1024;

    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        if (pSt->terminateThreads) break;

        int* snapArr     = (player == BLF_PLAYER_BLACK) ? snapBlack               : snapWhite;
        int* consumedArr = (player == BLF_PLAYER_BLACK) ? pSt->mwBlackFilesConsumed
                                                        : pSt->mwWhiteFilesConsumed;

        // Gather unconsumed writer files [consumed..snap) from each MW directory.
        // Explicit index enumeration keeps us away from any file the other thread
        // may currently be writing (its index >= snap by the after-close guarantee).
        char**   paths     = (char**)MemMalloc("xdimPaths", (size_t)kMaxFiles * sizeof(char*));
        int64_t* sizes     = (int64_t*)MemMalloc("xdimSizes", (size_t)kMaxFiles * sizeof(int64_t));
        if (!paths || !sizes)
            Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: alloc");

        int     numFiles   = 0;
        int64_t totalBytes = 0;

        for (int ti = 0; ti < pSt->numMergeWriters && numFiles < kMaxFiles; ti++)
        {
            for (int idx = consumedArr[ti]; idx < snapArr[ti] && numFiles < kMaxFiles; idx++)
            {
                char path[MAX_FULL_PATH_NAME];
                if (compress)
                    BLFZNameWriterFile(path, sizeof(path), pSt->mwDirectory[ti], player, idx);
                else
                    BLFNameWriterFile(path, sizeof(path), pSt->mwDirectory[ti], player, idx);

                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
                {
                    // Try the other compression variant (e.g. mixed-mode run)
                    if (compress)
                        BLFNameWriterFile(path, sizeof(path), pSt->mwDirectory[ti], player, idx);
                    else
                        BLFZNameWriterFile(path, sizeof(path), pSt->mwDirectory[ti], player, idx);
                    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
                        continue;  // empty flush was deleted — skip this index
                }
                int64_t sz = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
                paths[numFiles] = (char*)MemMalloc("xdimPath", strlen(path) + 1);
                if (!paths[numFiles])
                    Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: path alloc");
                strcpy(paths[numFiles], path);
                sizes[numFiles] = sz;
                totalBytes += sz;
                numFiles++;
            }
        }

        if (numFiles == 0)
        {
            MemFree(paths);
            MemFree(sizes);
            for (int ti = 0; ti < pSt->numMergeWriters; ti++)
                consumedArr[ti] = snapArr[ti];
            continue;
        }

        pSt->imergeTotalInputBytes[0] += (uint64_t)totalBytes;

        // Try to reserve space on the first merge drive (F:).
        int  destDirIdx    = -1;
        bool useTotalFlush = false;

        for (int d = 0; d < pSt->numMergeDirs; d++)
        {
            if (DriveReserve(pSt, pSt->mergeDirectory[d][0], totalBytes))
            {
                destDirIdx = d;
                break;
            }
        }
        if (destDirIdx < 0)
            useTotalFlush = true;

        if (useTotalFlush)
        {
            // F: is full.  Pull in all existing F: imerge files for this level+player
            // so the combined merge catches every possible cross-drive duplicate,
            // then flush everything to Y: to clear all fast drives at once.
            LoggerLog("DoCrossDriveIntermediateMerge: %s F: full — total flush to %c:\n",
                      BLFPlayerStr(player), pCtx->pConfig->storeDrive);

            for (int d = 0; d < pSt->numMergeDirs && numFiles < kMaxFiles; d++)
            {
                char        pat[MAX_FULL_PATH_NAME];
                uint64_t    iBytes = 0;
                char**      tmp    = (char**)MemMalloc("xdimTmp",
                                        (size_t)(kMaxFiles - numFiles) * sizeof(char*));
                uint64_t*   tmpSz  = (uint64_t*)MemMalloc("xdimTmpSz",
                                        (size_t)(kMaxFiles - numFiles) * sizeof(uint64_t));
                if (!tmp || !tmpSz)
                    Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: imerge enum");

                if (compress)
                    BLFZPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[d], level, player);
                else
                    BLFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[d], level, player);

                int extra = EnumerateByPattern(pat, tmp, kMaxFiles - numFiles, &iBytes, tmpSz);
                for (int k = 0; k < extra && numFiles < kMaxFiles; k++)
                {
                    paths[numFiles] = tmp[k];               // transfer ownership
                    sizes[numFiles] = (int64_t)tmpSz[k];
                    totalBytes     += (int64_t)tmpSz[k];
                    numFiles++;
                }
                MemFree(tmp);    // free array; elements now owned by paths[]
                MemFree(tmpSz);
            }

            // Reserve Y: worst-case (pre-dedup)
            if (!DriveReserve(pSt, pCtx->pConfig->storeDrive, totalBytes))
                Fatal(FATAL_DRIVE_SPACE,
                      "DoCrossDriveIntermediateMerge: total flush %s needs %.2f GB on %c:",
                      BLFPlayerStr(player),
                      totalBytes / (1024.0 * 1024.0 * 1024.0),
                      pCtx->pConfig->storeDrive);

            volatile LONG* pCount = (player == BLF_PLAYER_BLACK)
                ? (volatile LONG*)&pSt->storeMergeBlackFileCount
                : (volatile LONG*)&pSt->storeMergeWhiteFileCount;
            int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);

            char outPath[MAX_FULL_PATH_NAME];
            if (compress)
                BLFZNameImergeFile(outPath, sizeof(outPath), pSt->storeMergeDirectory,
                                   level, player, fileIdx);
            else
                BLFNameImergeFile(outPath, sizeof(outPath), pSt->storeMergeDirectory,
                                  level, player, fileIdx);

            LoggerLog("DoCrossDriveIntermediateMerge: total flush %s -> '%s' (%d files, %.2f GB)\n",
                      BLFPlayerStr(player), outPath, numFiles,
                      totalBytes / (1024.0 * 1024.0 * 1024.0));

            uint64_t unique = KWayMergeFiles(paths, numFiles, outPath,
                                              nullptr, compress, &pSt->terminateThreads);

            // Reclaim Y: overestimate
            int64_t actual = 0;
            if (compress)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(unique * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer));
            }
            DriveReclaim(pSt, pCtx->pConfig->storeDrive, totalBytes - actual);

            // Delete all inputs and reclaim their drive space
            for (int fi = 0; fi < numFiles; fi++)
            {
                DriveReclaim(pSt, paths[fi][0], sizes[fi]);
                DeleteFileA(paths[fi]);
                MemFree(paths[fi]);
            }

            // Clear F: imerge file counters for this player — all F: files were consumed
            for (int d = 0; d < pSt->numMergeDirs; d++)
            {
                if (player == BLF_PLAYER_BLACK) pSt->mergeFileBlackCount[d] = 0;
                else                            pSt->mergeFileWhiteCount[d] = 0;
            }

            LoggerLog("DoCrossDriveIntermediateMerge: total flush %s done (%llu unique)\n",
                      BLFPlayerStr(player), unique);
        }
        else
        {
            // Normal path: merge writer files from D:+E: -> single imerge on F:
            volatile LONG* pCount = (player == BLF_PLAYER_BLACK)
                ? (volatile LONG*)&pSt->mergeFileBlackCount[destDirIdx]
                : (volatile LONG*)&pSt->mergeFileWhiteCount[destDirIdx];
            int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);

            char outPath[MAX_FULL_PATH_NAME];
            if (compress)
                BLFZNameImergeFile(outPath, sizeof(outPath), pSt->mergeDirectory[destDirIdx],
                                   level, player, fileIdx);
            else
                BLFNameImergeFile(outPath, sizeof(outPath), pSt->mergeDirectory[destDirIdx],
                                  level, player, fileIdx);

            LoggerLog("DoCrossDriveIntermediateMerge: %s -> '%s' (%d files, %.2f GB)\n",
                      BLFPlayerStr(player), outPath, numFiles,
                      totalBytes / (1024.0 * 1024.0 * 1024.0));

            uint64_t unique = KWayMergeFiles(paths, numFiles, outPath,
                                              nullptr, compress, &pSt->terminateThreads);

            int64_t actual = 0;
            if (compress)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(unique * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer));
            }
            DriveReclaim(pSt, pSt->mergeDirectory[destDirIdx][0], totalBytes - actual);

            for (int fi = 0; fi < numFiles; fi++)
            {
                DriveReclaim(pSt, paths[fi][0], sizes[fi]);
                DeleteFileA(paths[fi]);
                MemFree(paths[fi]);
            }

            LoggerLog("DoCrossDriveIntermediateMerge: %s done (%llu unique)\n",
                      BLFPlayerStr(player), unique);
        }

        // Advance consumed pointers past the files we just merged
        for (int ti = 0; ti < pSt->numMergeWriters; ti++)
            consumedArr[ti] = snapArr[ti];

        MemFree(paths);
        MemFree(sizes);
    }

    pSt->imergeActive[0]          = 0;
    pSt->imergeTotalInputBytes[0] = 0;

    LeaveCriticalSection(&pSt->imergeCS);
}

// ============================================================================
// DoEndOfLevelMerge
//
// Runs two independent cascading merges: one for black-turn files, one for
// white-turn files.  Each produces a single sorted store file for level+1.
// ============================================================================

void DoEndOfLevelMerge(PSolveContext pCtx)
{
    POthelloLevelBlasterState  pSt            = pCtx->pState;
    POthelloLevelBlasterConfig pCfg           = pCtx->pConfig;
    int                        level          = (int)pSt->playLevel;
    int                        boardSize      = (int)pCfg->boardSize;
    bool                       compressOutput = (pCfg->compressMode != COMPRESS_NONE);

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;

    pSt->mergeTotalInputBytes[0] = pSt->mergeTotalInputBytes[1] = 0;
    pSt->mergeProgressBytes[0]   = pSt->mergeProgressBytes[1]   = 0;

    // ── Phase 1: enumerate files for both players (sequential, fast) ─────────
    // We scan first so mergeTotalInputBytes is known before the merge starts,
    // giving the stats listener an accurate denominator from the beginning.

    struct PlayerData {
        char**      inputPaths;
        uint64_t*   inputSizes;       // per-file sizes for ledger reclaim
        int         numFiles;
        uint64_t    inputBytes;
        uint64_t    unique;
        int64_t     storeReservation; // bytes pre-reserved on Y: for final output
        int64_t     actualBytes;      // actual bytes written to the output file
    };
    PlayerData data[2] = {};  // indexed by BLF_PLAYER_WHITE(0) / BLF_PLAYER_BLACK(1)

    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        data[player].inputPaths = (char**)MemMalloc("eolInputPaths",
                                                     (size_t)kMaxInputFiles * sizeof(char*));
        data[player].inputSizes = (uint64_t*)MemMalloc("eolInputSizes",
                                                         (size_t)kMaxInputFiles * sizeof(uint64_t));
        if (!data[player].inputPaths || !data[player].inputSizes)
            Fatal(FATAL_ALLOCATION_FAILED, "DoEndOfLevelMerge: cannot allocate path/size arrays");

        int      numFiles    = 0;
        uint64_t playerBytes = 0;
        char     pat[MAX_FULL_PATH_NAME];

        for (int i = 0; i < pSt->numMergeWriters && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            BLFPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                BLFZPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
            }
        }
        for (int i = 0; i < pSt->numMergeDirs && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                BLFZPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
            }
        }
        if (numFiles < kMaxInputFiles)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                BLFZPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
            }
        }

        data[player].numFiles   = numFiles;
        data[player].inputBytes = playerBytes;
    }

    // Set mergeTotalInputBytes per player as uncompressed record bytes.
    // mergeProgressBytes is incremented by sizeof(BOARD_KEY_DISK) per record, so the
    // denominator must be in the same units to give a valid percentage.
    {
        for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
        {
            uint64_t playerRecordBytes = 0;
            for (int i = 0; i < data[player].numFiles; i++)
            {
                BLFReader* r = BLFOpen(data[player].inputPaths[i]);
                if (r) { playerRecordBytes += BLFTrailer(r)->recordCount * sizeof(BOARD_KEY_DISK); BLFClose(&r); }
            }
            pSt->mergeTotalInputBytes[player] = playerRecordBytes;
        }
    }

    // ── Phase 1b: pre-reserve Y: store space for both players ────────────────
    // Cascade temp space is NOT reserved upfront — it is claimed per-group
    // inside CascadingMerge (filling F: first, spilling to Y: only as needed).
    // All reservations are atomic so both threads can run safely in parallel.

    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        PlayerData& pd      = data[player];
        pd.storeReservation = 0;
        if (pd.numFiles == 0) continue;

        // Reserve Y: for the final store output (worst case = full input size).
        int64_t storeNeeded = (pd.numFiles == 1) ? (int64_t)pd.inputSizes[0]
                                                  : (int64_t)pd.inputBytes;
        if (!DriveReserve(pSt, pCfg->storeDrive, storeNeeded))
            Fatal(FATAL_DRIVE_SPACE,
                  "EndOfLevelMerge: %s store output needs %.2f GB on %c: (%.2f GB available)",
                  BLFPlayerStr(player),
                  storeNeeded / (1024.0 * 1024.0 * 1024.0),
                  pCfg->storeDrive,
                  DriveAvailable(pSt, pCfg->storeDrive) / (1024.0 * 1024.0 * 1024.0));
        pd.storeReservation = storeNeeded;
    }

    // Ordered list of candidate temp dirs: merge drives (F:, etc.) first —
    // they are faster for writes and reads, and don't consume Y: bandwidth —
    // then storeMergeDirectory as last resort.  CascadingMerge picks the first
    // dir with ledger space per group, spreading temps across drives when one fills.
    const char* tempDirs[MAX_WRITER_DRIVES + 1];
    int         numTempDirs = 0;
    for (int d = 0; d < pSt->numMergeDirs; d++)
        tempDirs[numTempDirs++] = pSt->mergeDirectory[d];
    tempDirs[numTempDirs++] = pSt->storeMergeDirectory;

    // ── Phase 2: merge both players concurrently ─────────────────────────────
    // Each thread reads its own input files and writes a separate store file.
    // Progress is updated atomically so the stats display stays accurate.

    auto mergePlayer = [&](int player)
    {
        volatile int64_t* pProg = &pSt->mergeProgressBytes[player];
        PlayerData& pd = data[player];

        if (pd.numFiles == 0)
        {
            LoggerLog("EndOfLevelMerge: level %d %s -- no files\n",
                      level, BLFPlayerStr(player));
            MemFree(pd.inputPaths);
            MemFree(pd.inputSizes);
            return;
        }

        char outPath[MAX_FULL_PATH_NAME];
        if (compressOutput)
            BLFZNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                              boardSize, level + 1, player, 0);
        else
            BLFNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                             boardSize, level + 1, player, 0);

        if (pd.numFiles == 1)
        {
            if (!compressOutput)
            {
                BLFReader* r = BLFOpen(pd.inputPaths[0]);
                if (r) { pd.unique = BLFTrailer(r)->recordCount; BLFClose(&r); }
                if (!MoveFileExA(pd.inputPaths[0], outPath, MOVEFILE_COPY_ALLOWED))
                    Fatal(FATAL_FILE_OPEN,
                          "EndOfLevelMerge: cannot move '%s' -> '%s' (err %lu)",
                          pd.inputPaths[0], outPath, GetLastError());
                DriveReclaim(pSt, pd.inputPaths[0][0], (int64_t)pd.inputSizes[0]);
                int64_t actual = (int64_t)(pd.unique * sizeof(BOARD_KEY_DISK)
                                 + sizeof(BlasterFileTrailer));
                DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);
                InterlockedAdd64((volatile LONG64*)pProg, (LONG64)(pd.unique * sizeof(BOARD_KEY_DISK)));
            }
            else
            {
                // Compress (or re-compress) single input through streaming writer; KWayMergeFiles
                // calls BLFOpen which auto-detects .blf/.blfz from the magic value.
                pd.unique = KWayMergeFiles(pd.inputPaths, 1, outPath, pProg, true,
                                           &pSt->terminateThreads);
                DeleteFileA(pd.inputPaths[0]);
                DriveReclaim(pSt, pd.inputPaths[0][0], (int64_t)pd.inputSizes[0]);
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                int64_t actual = 0;
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
                DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);
            }
        }
        else
        {
            // Cascade temps are placed greedily per-group (F: first, Y: if needed).
            // When compressOutput is on, also compress intermediate cascade temps.
            int tempCount = 0;
            pd.unique = CascadingMerge(pd.inputPaths, pd.numFiles, tempDirs, numTempDirs,
                                        outPath, &tempCount, level, player, pProg, pCtx,
                                        compressOutput, compressOutput);

            // Return Y: overestimate; use actual file size for compressed output.
            int64_t actual;
            if (compressOutput)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                actual = 0;
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(pd.unique * sizeof(BOARD_KEY_DISK)
                         + (pd.unique > 0 ? sizeof(BlasterFileTrailer) : 0));
            }
            DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);

            // Reclaim source-file drive space as each input is deleted.
            for (int i = 0; i < pd.numFiles; i++)
            {
                DriveReclaim(pSt, pd.inputPaths[i][0], (int64_t)pd.inputSizes[i]);
                DeleteFileA(pd.inputPaths[i]);
            }
        }

        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                pd.actualBytes = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
        }
        if (pd.unique > 0)
            InterlockedIncrement((volatile LONG*)&pSt->levelStats[level].mergeFilesWritten);

        LoggerLog("EndOfLevelMerge: level %d %s -> '%s'  (%llu unique boards)\n",
                  level, BLFPlayerStr(player), outPath, pd.unique);

        for (int i = 0; i < pd.numFiles; i++)
            MemFree(pd.inputPaths[i]);
        MemFree(pd.inputPaths);
        MemFree(pd.inputSizes);
    };

    std::thread blackThread([&] { mergePlayer(BLF_PLAYER_BLACK); });
    std::thread whiteThread([&] { mergePlayer(BLF_PLAYER_WHITE); });
    blackThread.join();
    whiteThread.join();

    // ── Finalize stats ────────────────────────────────────────────────────────
    uint64_t blackUnique = data[BLF_PLAYER_BLACK].unique;
    uint64_t whiteUnique = data[BLF_PLAYER_WHITE].unique;
    uint64_t totalUnique = blackUnique + whiteUnique;
    uint64_t outBytes    = totalUnique * sizeof(BOARD_KEY_DISK)
                         + (blackUnique > 0 ? sizeof(BlasterFileTrailer) : 0)
                         + (whiteUnique > 0 ? sizeof(BlasterFileTrailer) : 0);

    pSt->levelStats[level].mrgDupsRemoved =
        pSt->levelStats[level].boardsWrittenToDisk - totalUnique;
    pSt->levelStats[level].mergeBytes       = outBytes;
    pSt->levelStats[level].mergeActualBytes = (uint64_t)(data[BLF_PLAYER_BLACK].actualBytes
                                                        + data[BLF_PLAYER_WHITE].actualBytes);
}
