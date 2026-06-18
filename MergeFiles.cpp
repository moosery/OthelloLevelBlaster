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

    int     numGroups       = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
    char**  tempPaths       = (char**)MemMalloc("cascadeTempPaths",
                                                  (size_t)numGroups * sizeof(char*));
    int64_t* tempActualSizes = pSt
        ? (int64_t*)MemMalloc("cascadeTempSizes", (size_t)numGroups * sizeof(int64_t))
        : nullptr;
    if (!tempPaths || (pSt && !tempActualSizes))
        Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp arrays");

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

        // Sum input file sizes for this group so we can reclaim the overestimate.
        int64_t groupBytes = 0;
        if (pSt)
        {
            for (int k = start; k < start + groupSize; k++)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExA(inputPaths[k], GetFileExInfoStandard, &fad))
                    groupBytes += ((int64_t)fad.nFileSizeHigh << 32)
                                | (int64_t)fad.nFileSizeLow;
            }
        }

        if (pSt) pSt->cascadeGroupProgressBytes[player] = 0;

        char tempPath[MAX_FULL_PATH_NAME];
        BLFNameCascadeTemp(tempPath, sizeof(tempPath), workDir, level, player, (*pTempCount)++);

        uint64_t tempUnique = KWayMergeFiles(inputPaths + start, groupSize, tempPath,
                                              pSt ? &pSt->cascadeGroupProgressBytes[player]
                                                  : nullptr);

        if (pSt)
        {
            int64_t tempActual = (int64_t)(tempUnique * sizeof(BOARD_KEY_DISK)
                                 + sizeof(BlasterFileTrailer));
            tempActualSizes[numTemps] = tempActual;
            // Return the portion of the pre-reserved space that dedup saved.
            DriveReclaim(pSt, workDir[0], groupBytes - tempActual);
        }

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
        // Reclaim temp drive space as each temp file is deleted.
        if (pSt) DriveReclaim(pSt, workDir[0], tempActualSizes[i]);
        DeleteFileA(tempPaths[i]);
        MemFree(tempPaths[i]);
    }
    MemFree(tempPaths);
    if (tempActualSizes) MemFree(tempActualSizes);

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

    // Debit the NVMe ledger for bytes just written, then check threshold.
    char driveLetter = pSt->mwDirectory[ti][0];
    DriveDebit(pSt, driveLetter, (int64_t)fileBytes);

    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        if (pSt->writerDriveStats[i].driveLetter == driveLetter)
        {
            pSt->writerDriveStats[i].levelFilesWritten += filesCreated;
            pSt->writerDriveStats[i].levelBytesWritten += fileBytes;
            if (DriveAvailable(pSt, driveLetter) < (int64_t)pSt->writerDriveStats[i].threshold)
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
            int64_t  batchBytes = 0;
            for (int i = batchStart; i < batchStart + batchCount; i++)
                batchBytes += (int64_t)fileSizes[i];

            // Reserve dest space: try each merge dir in order, fall back to Y:.
            int         dirIdx      = -1;
            bool        useStoreMrg = true;
            for (int d = 0; d < pSt->numMergeDirs; d++)
            {
                if (DriveReserve(pSt, pSt->mergeDirectory[d][0], batchBytes))
                {
                    dirIdx      = d;
                    useStoreMrg = false;
                    break;
                }
            }
            if (useStoreMrg &&
                !DriveReserve(pSt, pCtx->pConfig->storeDrive, batchBytes))
                Fatal(FATAL_DRIVE_SPACE,
                      "IntermediateMerge: no drive has %.2f GB for mw[%d] %s batch",
                      batchBytes / (1024.0 * 1024.0 * 1024.0), mwIdx, playerStr);

            const char* destDir    = useStoreMrg ? pSt->storeMergeDirectory
                                                  : pSt->mergeDirectory[dirIdx];
            char        destLetter = useStoreMrg ? pCtx->pConfig->storeDrive
                                                 : pSt->mergeDirectory[dirIdx][0];

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
            int64_t  actual = (int64_t)(unique * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer));
            DriveReclaim(pSt, destLetter, batchBytes - actual);

            LoggerLog("IntermediateMerge: [%d..%d] %s -> '%s'  (%llu unique boards)\n",
                      batchStart, batchStart + batchCount - 1, playerStr, outPath, unique);

            for (int i = batchStart; i < batchStart + batchCount; i++)
            {
                DriveReclaim(pSt, inputPaths[i][0], (int64_t)fileSizes[i]);
                DeleteFileA(inputPaths[i]);
                MemFree(inputPaths[i]);
            }

            pSt->imergeDoneInputBytes[mwIdx] += (uint64_t)batchBytes;
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
        char**      inputPaths;
        uint64_t*   inputSizes;       // per-file sizes for ledger reclaim
        int         numFiles;
        uint64_t    inputBytes;
        uint64_t    unique;
        char        tempDrive;        // drive for cascade temps (0 = no cascade)
        const char* tempDir;          // path for cascade temp directory
        int64_t     storeReservation; // bytes pre-reserved on Y: for final output
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
        }
        for (int i = 0; i < pSt->numMergeDirs && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
        }
        if (numFiles < kMaxInputFiles)
        {
            uint64_t d = 0;
            BLFPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
        }

        data[player].numFiles   = numFiles;
        data[player].inputBytes = playerBytes;
        pSt->mergeTotalInputBytes += playerBytes;
    }

    // ── Phase 1b: pre-reserve drive space for both players before any thread starts ──
    // Temp space (cascade intermediates) is reserved on F: if it fits, else Y:.
    // Final output space on Y: is reserved as worst-case (inputBytes) and corrected
    // after each merge by reclaiming the dedup savings.
    // All reservations are atomic so both threads can run safely in parallel.

    for (int player = BLF_PLAYER_WHITE; player <= BLF_PLAYER_BLACK; player++)
    {
        PlayerData& pd     = data[player];
        pd.tempDrive       = 0;
        pd.tempDir         = nullptr;
        pd.storeReservation = 0;

        if (pd.numFiles == 0) continue;

        int64_t inputBytes = (int64_t)pd.inputBytes;

        if (pd.numFiles > 1)
        {
            // Try each merge dir (F:, etc.) for cascade temp space.
            bool gotMerge = false;
            for (int d = 0; d < pSt->numMergeDirs; d++)
            {
                if (DriveReserve(pSt, pSt->mergeDirectory[d][0], inputBytes))
                {
                    pd.tempDrive = pSt->mergeDirectory[d][0];
                    pd.tempDir   = pSt->mergeDirectory[d];
                    gotMerge     = true;
                    break;
                }
            }
            if (!gotMerge)
            {
                // Fall back to Y: for cascade temps.
                if (!DriveReserve(pSt, pCfg->storeDrive, inputBytes))
                    Fatal(FATAL_DRIVE_SPACE,
                          "EndOfLevelMerge: %s cascade temps need %.2f GB — no drive has room",
                          BLFPlayerStr(player),
                          inputBytes / (1024.0 * 1024.0 * 1024.0));
                pd.tempDrive = pCfg->storeDrive;
                pd.tempDir   = pSt->storeMergeDirectory;
                LoggerLog("EndOfLevelMerge: %s cascade temps on %c: (%.2f GB, merge drives full)\n",
                          BLFPlayerStr(player), pCfg->storeDrive,
                          inputBytes / (1024.0 * 1024.0 * 1024.0));
            }
        }

        // Reserve Y: for the final store output (worst case = full input size).
        int64_t storeNeeded = (pd.numFiles == 1) ? (int64_t)pd.inputSizes[0] : inputBytes;
        if (!DriveReserve(pSt, pCfg->storeDrive, storeNeeded))
            Fatal(FATAL_DRIVE_SPACE,
                  "EndOfLevelMerge: %s store output needs %.2f GB on %c: (%.2f GB available)",
                  BLFPlayerStr(player),
                  storeNeeded / (1024.0 * 1024.0 * 1024.0),
                  pCfg->storeDrive,
                  DriveAvailable(pSt, pCfg->storeDrive) / (1024.0 * 1024.0 * 1024.0));
        pd.storeReservation = storeNeeded;
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
            MemFree(pd.inputSizes);
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
            // MoveFileExA deleted the source; reclaim its drive space.
            DriveReclaim(pSt, pd.inputPaths[0][0], (int64_t)pd.inputSizes[0]);
            // Return any Y: overestimate (exact size was reserved, so this is usually 0).
            int64_t actual = (int64_t)(pd.unique * sizeof(BOARD_KEY_DISK)
                             + sizeof(BlasterFileTrailer));
            DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);
            InterlockedAdd64((volatile LONG64*)pProg, (LONG64)(pd.unique * sizeof(BOARD_KEY_DISK)));
        }
        else
        {
            // tempDir was chosen and reserved in Phase 1b.
            int tempCount = 0;
            pd.unique = CascadingMerge(pd.inputPaths, pd.numFiles, pd.tempDir, outPath,
                                        &tempCount, level, player, pProg, pCtx);

            // Return Y: overestimate (dedup reduces actual output vs. worst-case reservation).
            int64_t actual = (int64_t)(pd.unique * sizeof(BOARD_KEY_DISK)
                             + (pd.unique > 0 ? sizeof(BlasterFileTrailer) : 0));
            DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);

            // Reclaim source-file drive space as each input is deleted.
            for (int i = 0; i < pd.numFiles; i++)
            {
                DriveReclaim(pSt, pd.inputPaths[i][0], (int64_t)pd.inputSizes[i]);
                DeleteFileA(pd.inputPaths[i]);
            }
        }

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
    pSt->levelStats[level].mergeBytes = outBytes;
}
