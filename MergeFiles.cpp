#include "MergeFiles.h"
#include "BlasterFile.h"
#include "BlasterFileName.h"
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

// Returns the first merge directory that can fit neededBytes (worst-case output size),
// accounting for space already committed by other in-flight intermediate merge batches.
static const char* SelectMergeDestination(POthelloLevelBlasterState pSt,
                                           uint64_t neededBytes, int* pDirIdx)
{
    *pDirIdx = -1;
    for (int i = 0; i < pSt->numMergeDirs; i++)
    {
        char root[4] = { pSt->mergeDirectory[i][0], ':', '\\', '\0' };
        ULARGE_INTEGER freeAvail = {};
        GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);

        volatile LONG64* pRes = (volatile LONG64*)&pSt->mergeDirReservedBytes[i];
        LONG64 oldRes = InterlockedCompareExchange64(pRes, 0, 0);
        for (;;)
        {
            int64_t effective = (int64_t)freeAvail.QuadPart - oldRes;
            if (effective < (int64_t)neededBytes) break;
            LONG64 newRes = oldRes + (LONG64)neededBytes;
            LONG64 got    = InterlockedCompareExchange64(pRes, newRes, oldRes);
            if (got == oldRes) { *pDirIdx = i; return pSt->mergeDirectory[i]; }
            oldRes = got;
        }
    }
    return nullptr;
}

// File-based k-way merge of player-homogeneous files (all same player).
// Reads/writes BOARD_KEY_DISK (16 bytes).  Deduplicates on (cellsInUse, cellColors).
static uint64_t KWayMergeFiles(char** inputPaths, int numInputs, const char* outputPath,
                                volatile int64_t* pProgressBytes)
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

    BLFWriter*     pw      = BLFWriterOpen(outputPath);
    BOARD_KEY_DISK lastKey = {};
    bool           hasLast = false;

    while (!heap.empty())
    {
        MergeHead top = heap.top();
        heap.pop();

        bool isDup = hasLast
                     && top.key.ullCellsInUse == lastKey.ullCellsInUse
                     && top.key.ullCellColors == lastKey.ullCellColors;
        if (!isDup)
        {
            BLFWriterRecord(pw, &top.key);
            lastKey = top.key;
            hasLast = true;
            if (pProgressBytes)
                InterlockedAdd64((volatile LONG64*)pProgressBytes, (LONG64)sizeof(BOARD_KEY_DISK));
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

    return BLFWriterClose(pw);
}

// pCtx is non-null only on the outer call; nullptr is passed for the recursive final-pass
// call so cascade tracking is not re-entered.
static uint64_t CascadingMerge(char** inputPaths, int numInputs,
                                 const char* workDir, const char* finalOutPath,
                                 int* pTempCount, int level, int player,
                                 volatile int64_t* pProgressBytes,
                                 PSolveContext pCtx)
{
    if (numInputs <= MAX_MERGE_FANIN)
        return KWayMergeFiles(inputPaths, numInputs, finalOutPath, pProgressBytes);

    POthelloLevelBlasterState pSt = pCtx ? pCtx->pState : nullptr;

    int    numGroups = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
    char** tempPaths = (char**)MemMalloc("cascadeTempPaths", (size_t)numGroups * sizeof(char*));
    if (!tempPaths)
        Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path array");

    if (pSt)
    {
        pSt->cascadeNumGroups[player]          = numGroups;
        pSt->cascadeGroupsDone[player]         = 0;
        pSt->cascadeGroupProgressBytes[player] = 0;
        pSt->cascadeActive[player]             = true;
    }

    int numTemps = 0;
    for (int g = 0; g < numGroups; g++)
    {
        int start     = g * MAX_MERGE_FANIN;
        int groupSize = (std::min)(MAX_MERGE_FANIN, numInputs - start);

        if (pSt) pSt->cascadeGroupProgressBytes[player] = 0;

        char tempPath[MAX_FULL_PATH_NAME];
        BLFNameCascadeTemp(tempPath, sizeof(tempPath), workDir, level, player, (*pTempCount)++);

        KWayMergeFiles(inputPaths + start, groupSize, tempPath,
                       pSt ? &pSt->cascadeGroupProgressBytes[player] : nullptr);

        tempPaths[numTemps] = (char*)MemMalloc("cascadeTempPath", strlen(tempPath) + 1);
        if (!tempPaths[numTemps])
            Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path");
        strcpy(tempPaths[numTemps], tempPath);
        numTemps++;

        if (pSt) pSt->cascadeGroupsDone[player]++;
    }

    if (pSt) pSt->cascadeActive[player] = false;

    uint64_t unique = CascadingMerge(tempPaths, numTemps, workDir, finalOutPath,
                                      pTempCount, level, player, pProgressBytes,
                                      nullptr);

    for (int i = 0; i < numTemps; i++)
    {
        DeleteFileA(tempPaths[i]);
        MemFree(tempPaths[i]);
    }
    MemFree(tempPaths);

    return unique;
}

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

    BOARD_KEY_DISK* mwBuf = (BOARD_KEY_DISK*)pSt->pMWBuffer[ti];

    uint64_t blackCount = 0, whiteCount = 0;
    int      filesCreated = 0;
    uint64_t fileBytes    = 0;

    // --- Black stream: merge sorted runs from the top of the buffer ---
    if (hasBlack)
    {
        char blackPath[MAX_FULL_PATH_NAME];
        BLFNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                          BLF_PLAYER_BLACK, pSt->mwBlackFileCount[ti]++);
        BLFWriter* pw = BLFWriterOpen(blackPath);

        std::priority_queue<InMemDiskHead, std::vector<InMemDiskHead>, InMemDiskHeadGreater> heap;
        for (int s = 0; s < pSt->mwBlackSegCount[ti]; s++)
        {
            const BOARD_KEY_DISK* pSeg = mwBuf + pSt->mwBlackSegOffset[ti][s];
            if (pSt->mwBlackSegSize[ti][s] > 0)
                heap.push({ pSeg, pSeg + pSt->mwBlackSegSize[ti][s] });
        }

        BOARD_KEY_DISK lastKey = {}; bool hasLast = false;
        while (!heap.empty())
        {
            InMemDiskHead top = heap.top(); heap.pop();
            bool dup = hasLast
                       && top.pCurrent->ullCellsInUse == lastKey.ullCellsInUse
                       && top.pCurrent->ullCellColors == lastKey.ullCellColors;
            if (!dup) { BLFWriterRecord(pw, top.pCurrent); lastKey = *top.pCurrent; hasLast = true; }
            if (++top.pCurrent < top.pEnd) heap.push(top);
        }

        blackCount = BLFWriterClose(pw);
        if (blackCount == 0) { DeleteFileA(blackPath); pSt->mwBlackFileCount[ti]--; }
        else { fileBytes += blackCount * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer); filesCreated++; }
    }

    // --- White stream: merge sorted runs from the bottom of the buffer ---
    if (hasWhite)
    {
        char whitePath[MAX_FULL_PATH_NAME];
        BLFNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                          BLF_PLAYER_WHITE, pSt->mwWhiteFileCount[ti]++);
        BLFWriter* pw = BLFWriterOpen(whitePath);

        std::priority_queue<InMemDiskHead, std::vector<InMemDiskHead>, InMemDiskHeadGreater> heap;
        for (int s = 0; s < pSt->mwWhiteSegCount[ti]; s++)
        {
            const BOARD_KEY_DISK* pSeg = mwBuf + pSt->mwWhiteSegOffset[ti][s];
            if (pSt->mwWhiteSegSize[ti][s] > 0)
                heap.push({ pSeg, pSeg + pSt->mwWhiteSegSize[ti][s] });
        }

        BOARD_KEY_DISK lastKey = {}; bool hasLast = false;
        while (!heap.empty())
        {
            InMemDiskHead top = heap.top(); heap.pop();
            bool dup = hasLast
                       && top.pCurrent->ullCellsInUse == lastKey.ullCellsInUse
                       && top.pCurrent->ullCellColors == lastKey.ullCellColors;
            if (!dup) { BLFWriterRecord(pw, top.pCurrent); lastKey = *top.pCurrent; hasLast = true; }
            if (++top.pCurrent < top.pEnd) heap.push(top);
        }

        whiteCount = BLFWriterClose(pw);
        if (whiteCount == 0) { DeleteFileA(whitePath); pSt->mwWhiteFileCount[ti]--; }
        else { fileBytes += whiteCount * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer); filesCreated++; }
    }

    // Reset segment tracking for this thread
    pSt->mwBlackSegCount[ti]   = 0;
    pSt->mwBlackBoardsUsed[ti] = 0;
    pSt->mwWhiteSegCount[ti]   = 0;
    pSt->mwWhiteBoardsUsed[ti] = 0;

    uint64_t unique = blackCount + whiteCount;
    pSt->levelStats[level].boardsWrittenToDisk += unique;
    pSt->levelStats[level].mwFilesCreated      += filesCreated;
    pSt->levelStats[level].mwBytes             += fileBytes;

    // Check drive space; trigger intermediate merge (NVMe -> HDD) if low
    char driveRoot[4] = { pSt->mwDirectory[ti][0], ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};
    GetDiskFreeSpaceExA(driveRoot, &freeAvail, nullptr, nullptr);

    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        if (pSt->writerDriveStats[i].driveLetter == pSt->mwDirectory[ti][0])
        {
            pSt->writerDriveStats[i].levelFilesWritten += filesCreated;
            pSt->writerDriveStats[i].levelBytesWritten += fileBytes;
            pSt->writerDriveStats[i].lastFreeBytes      = freeAvail.QuadPart;
            if (freeAvail.QuadPart < pSt->writerDriveStats[i].threshold)
                DoIntermediateMerge(ti, pCtx);
            break;
        }
    }
}

// ============================================================================
// DoIntermediateMerge
//
// Runs two separate passes (black then white) over the files in the MW
// directory.  Each pass k-way merges up to MAX_MERGE_FANIN files at a time
// into player-specific output files on the merge (medium) drive.
// ============================================================================

void DoIntermediateMerge(int mwIdx, PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt   = pCtx->pState;
    const char*               mwDir = pSt->mwDirectory[mwIdx];
    int                       level = (int)pSt->playLevel;

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;

    // Accumulate total input bytes across both players for progress tracking
    uint64_t totalInputBytes = 0;

    // First pass: enumerate all writer files to get the total byte count for
    // the progress display, then process each player separately.
    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        char pattern[MAX_FULL_PATH_NAME];
        BLFPatternWriterFiles(pattern, sizeof(pattern), mwDir, player);

        char**    inputPaths = (char**)MemMalloc("mergeInputPaths",
                                                  (size_t)kMaxInputFiles * sizeof(char*));
        uint64_t* fileSizes  = (uint64_t*)MemMalloc("mergeFileSizes",
                                                      (size_t)kMaxInputFiles * sizeof(uint64_t));
        if (!inputPaths || !fileSizes)
            Fatal(FATAL_ALLOCATION_FAILED, "DoIntermediateMerge: cannot allocate arrays");

        uint64_t playerBytes = 0;
        int numFiles = EnumerateByPattern(pattern, inputPaths, kMaxInputFiles,
                                          &playerBytes, fileSizes);
        totalInputBytes += playerBytes;

        if (numFiles == 0)
        {
            MemFree(fileSizes);
            MemFree(inputPaths);
            continue;
        }

        const char* playerStr = BLFPlayerStr(player);
        LoggerLog("IntermediateMerge: mw[%d] %c: %s -- %d files, %.2f GB\n",
                  mwIdx, mwDir[0], playerStr, numFiles,
                  (double)playerBytes / (1024.0 * 1024.0 * 1024.0));

        // Publish progress state once (on first player that has files)
        if (pSt->imergeTotalInputBytes[mwIdx] == 0)
        {
            pSt->imergeTotalInputBytes[mwIdx] = totalInputBytes;
            pSt->imergeDoneInputBytes[mwIdx]  = 0;
            pSt->imergeActive[mwIdx]          = 1;
        }
        // Update total in case white already ran and we're now on black
        pSt->imergeTotalInputBytes[mwIdx] += playerBytes;

        int batchStart = 0;
        while (batchStart < numFiles)
        {
            int      batchCount = (std::min)(MAX_MERGE_FANIN, numFiles - batchStart);
            uint64_t batchBytes = 0;
            for (int i = batchStart; i < batchStart + batchCount; i++)
                batchBytes += fileSizes[i];

            int         dirIdx      = -1;
            const char* destDir     = SelectMergeDestination(pSt, batchBytes, &dirIdx);
            bool        useStoreMrg = (destDir == nullptr);
            if (useStoreMrg)
                destDir = pSt->storeMergeDirectory;

            int fileIdx;
            if (useStoreMrg)
            {
                volatile LONG* pCount = (player == BLF_PLAYER_BLACK)
                    ? (volatile LONG*)&pSt->storeMergeBlackFileCount
                    : (volatile LONG*)&pSt->storeMergeWhiteFileCount;
                fileIdx = (int)InterlockedExchangeAdd(pCount, 1);
            }
            else
            {
                volatile LONG* pCount = (player == BLF_PLAYER_BLACK)
                    ? (volatile LONG*)&pSt->mergeFileBlackCount[dirIdx]
                    : (volatile LONG*)&pSt->mergeFileWhiteCount[dirIdx];
                fileIdx = (int)InterlockedExchangeAdd(pCount, 1);
            }

            char outPath[MAX_FULL_PATH_NAME];
            BLFNameImergeFile(outPath, sizeof(outPath), destDir, level, player, fileIdx);

            uint64_t unique = KWayMergeFiles(inputPaths + batchStart, batchCount, outPath, nullptr);

            if (!useStoreMrg && dirIdx >= 0)
                InterlockedAdd64((volatile LONG64*)&pSt->mergeDirReservedBytes[dirIdx],
                                 -(LONG64)batchBytes);

            LoggerLog("IntermediateMerge: [%d..%d] %s -> '%s'  (%llu unique boards)\n",
                      batchStart, batchStart + batchCount - 1, playerStr, outPath, unique);

            for (int i = batchStart; i < batchStart + batchCount; i++)
            {
                DeleteFileA(inputPaths[i]);
                MemFree(inputPaths[i]);
            }

            pSt->imergeDoneInputBytes[mwIdx] += batchBytes;
            batchStart += batchCount;
        }

        MemFree(fileSizes);
        MemFree(inputPaths);
    }

    pSt->imergeActive[mwIdx] = 0;
    // Reset so the next intermediate merge re-initialises the total correctly
    pSt->imergeTotalInputBytes[mwIdx] = 0;

    pSt->mwBlackFileCount[mwIdx] = 0;
    pSt->mwWhiteFileCount[mwIdx] = 0;
}

// ============================================================================
// DoEndOfLevelMerge
//
// Runs two independent cascading merges: one for black-turn files, one for
// white-turn files.  Each produces a single sorted store file for level+1.
// ============================================================================

void DoEndOfLevelMerge(PSolveContext pCtx)
{
    POthelloLevelBlasterState  pSt       = pCtx->pState;
    POthelloLevelBlasterConfig pCfg      = pCtx->pConfig;
    int                        level     = (int)pSt->playLevel;
    int                        boardSize = (int)pCfg->boardSize;

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;

    pSt->mergeTotalInputBytes = 0;
    pSt->mergeProgressBytes   = 0;

    // ── Phase 1: enumerate files for both players (sequential, fast) ─────────
    // We scan first so mergeTotalInputBytes is known before the merge starts,
    // giving the stats listener an accurate denominator from the beginning.

    struct PlayerData {
        char**   inputPaths;
        int      numFiles;
        uint64_t inputBytes;
        uint64_t unique;
    };
    PlayerData data[2] = {};  // indexed by BLF_PLAYER_WHITE(0) / BLF_PLAYER_BLACK(1)

    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        data[player].inputPaths = (char**)MemMalloc("eolInputPaths",
                                                     (size_t)kMaxInputFiles * sizeof(char*));
        if (!data[player].inputPaths)
            Fatal(FATAL_ALLOCATION_FAILED, "DoEndOfLevelMerge: cannot allocate path array");

        int      numFiles       = 0;
        uint64_t playerBytes    = 0;
        char     pat[MAX_FULL_PATH_NAME];

        for (int i = 0; i < pSt->numMergeWriters && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            BLFPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d);
            playerBytes += d;
        }
        for (int i = 0; i < pSt->numMergeDirs && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d);
            playerBytes += d;
        }
        if (numFiles < kMaxInputFiles)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d);
            playerBytes += d;
        }

        data[player].numFiles  = numFiles;
        data[player].inputBytes = playerBytes;
        pSt->mergeTotalInputBytes += playerBytes;
    }

    // ── Phase 2: merge both players concurrently ─────────────────────────────
    // Each thread reads its own input files and writes a separate store file.
    // Progress is updated atomically so the stats display stays accurate.

    volatile int64_t* pProg = &pSt->mergeProgressBytes;

    auto mergePlayer = [&](int player)
    {
        PlayerData& pd = data[player];

        if (pd.numFiles == 0)
        {
            LoggerLog("EndOfLevelMerge: level %d %s -- no files\n",
                      level, BLFPlayerStr(player));
            MemFree(pd.inputPaths);
            return;
        }

        char outPath[MAX_FULL_PATH_NAME];
        BLFNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                         boardSize, level + 1, player, 0);

        if (pd.numFiles == 1)
        {
            BLFReader* r = BLFOpen(pd.inputPaths[0]);
            if (r) { pd.unique = BLFTrailer(r)->recordCount; BLFClose(&r); }
            if (!MoveFileExA(pd.inputPaths[0], outPath, MOVEFILE_COPY_ALLOWED))
                Fatal(FATAL_FILE_OPEN,
                      "EndOfLevelMerge: cannot move '%s' -> '%s' (err %lu)",
                      pd.inputPaths[0], outPath, GetLastError());
            InterlockedAdd64((volatile LONG64*)pProg, (LONG64)(pd.unique * sizeof(BOARD_KEY_DISK)));
        }
        else
        {
            const char* tempDir = pSt->storeMergeDirectory;
            if (pSt->numMergeDirs > 0)
            {
                char root[4] = { pSt->mergeDirectory[0][0], ':', '\\', '\0' };
                ULARGE_INTEGER freeAvail = {};
                GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);
                if (freeAvail.QuadPart >= pd.inputBytes)
                    tempDir = pSt->mergeDirectory[0];
                else
                    LoggerLog("EndOfLevelMerge: %c: free %.2f GB < %.2f GB needed for %s cascade"
                              " -- using store merge dir\n",
                              pSt->mergeDirectory[0][0],
                              freeAvail.QuadPart  / (1024.0 * 1024.0 * 1024.0),
                              pd.inputBytes        / (1024.0 * 1024.0 * 1024.0),
                              BLFPlayerStr(player));
            }
            int tempCount = 0;
            pd.unique = CascadingMerge(pd.inputPaths, pd.numFiles, tempDir, outPath,
                                        &tempCount, level, player, pProg, pCtx);
            for (int i = 0; i < pd.numFiles; i++)
                DeleteFileA(pd.inputPaths[i]);
        }

        LoggerLog("EndOfLevelMerge: level %d %s -> '%s'  (%llu unique boards)\n",
                  level, BLFPlayerStr(player), outPath, pd.unique);

        for (int i = 0; i < pd.numFiles; i++)
            MemFree(pd.inputPaths[i]);
        MemFree(pd.inputPaths);
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
    pSt->levelStats[level].mergeBytes = outBytes;
}
