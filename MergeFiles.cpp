#include "MergeFiles.h"
#include "BlasterFile.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>
#include <algorithm>
#include <queue>
#include <vector>

// ============================================================================
// Min-heap entry for file-based k-way merge
// ============================================================================

struct MergeHead
{
    BOARD_KEY  key;
    BLFReader* pReader;
};

struct MergeHeadGreater
{
    bool operator()(const MergeHead& a, const MergeHead& b) const
    {
        return BoardKeyCompare(&a.key, &b.key) > 0;
    }
};

// ============================================================================
// Min-heap entry for in-memory k-way merge
// ============================================================================

struct InMemHead
{
    const BOARD_KEY* pCurrent;
    const BOARD_KEY* pEnd;
};

struct InMemHeadGreater
{
    bool operator()(const InMemHead& a, const InMemHead& b) const
    {
        return BoardKeyCompare(a.pCurrent, b.pCurrent) > 0;
    }
};

// ============================================================================
// Helpers
// ============================================================================

static int EnumerateWriterFiles(const char* dir, char** outPaths, int maxPaths,
                                 uint64_t* pTotalBytes)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\*.bin", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { *pTotalBytes = 0; return 0; }

    int count    = 0;
    *pTotalBytes = 0;
    do
    {
        if (count >= maxPaths) break;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        outPaths[count] = (char*)MemMalloc("mergePath", strlen(full) + 1);
        if (!outPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateWriterFiles: cannot allocate path");
        strcpy(outPaths[count], full);
        ULARGE_INTEGER sz;
        sz.LowPart  = fd.nFileSizeLow;
        sz.HighPart = (DWORD)fd.nFileSizeHigh;
        *pTotalBytes += sz.QuadPart;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

static const char* SelectMergeDestination(POthelloLevelBlasterState pSt, uint64_t neededBytes)
{
    for (int i = 0; i < pSt->numMergeDirs; i++)
    {
        char root[4] = { pSt->mergeDirectory[i][0], ':', '\\', '\0' };
        ULARGE_INTEGER freeAvail = {};
        GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);
        if (freeAvail.QuadPart >= neededBytes)
            return pSt->mergeDirectory[i];
    }
    return nullptr;
}

static uint64_t KWayMergeFiles(char** inputPaths, int numInputs, const char* outputPath,
                                uint64_t* pProgressBytes)
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
        BOARD_KEY first;
        if (BLFRead(r, &first, 1) == 1)
            heap.push({ first, r });
        else
            BLFClose(&r);
    }

    BLFWriter* pw      = BLFWriterOpen(outputPath);
    BOARD_KEY  lastKey = {};
    bool       hasLast = false;

    while (!heap.empty())
    {
        MergeHead top = heap.top();
        heap.pop();

        if (!hasLast || BoardKeyCompare(&top.key, &lastKey) != 0)
        {
            BLFWriterRecord(pw, &top.key);
            lastKey = top.key;
            hasLast = true;
            if (pProgressBytes) *pProgressBytes += sizeof(BOARD_KEY);
        }

        BOARD_KEY next;
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

static uint64_t CascadingMerge(char** inputPaths, int numInputs,
                                 const char* workDir, const char* finalOutPath,
                                 int* pTempCount, int level, uint64_t* pProgressBytes)
{
    if (numInputs <= MAX_MERGE_FANIN)
        return KWayMergeFiles(inputPaths, numInputs, finalOutPath, pProgressBytes);

    int    numGroups = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
    char** tempPaths = (char**)MemMalloc("cascadeTempPaths", (size_t)numGroups * sizeof(char*));
    if (!tempPaths)
        Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path array");

    int numTemps = 0;
    for (int g = 0; g < numGroups; g++)
    {
        int start     = g * MAX_MERGE_FANIN;
        int groupSize = (std::min)(MAX_MERGE_FANIN, numInputs - start);

        char tempPath[MAX_FULL_PATH_NAME];
        snprintf(tempPath, sizeof(tempPath), "%s\\cascade_temp_L%03d_%04d.bin",
                 workDir, level, (*pTempCount)++);

        KWayMergeFiles(inputPaths + start, groupSize, tempPath, nullptr);  // intermediate: don't count toward progress

        tempPaths[numTemps] = (char*)MemMalloc("cascadeTempPath", strlen(tempPath) + 1);
        if (!tempPaths[numTemps])
            Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path");
        strcpy(tempPaths[numTemps], tempPath);
        numTemps++;
    }

    uint64_t unique = CascadingMerge(tempPaths, numTemps, workDir, finalOutPath, pTempCount, level, pProgressBytes);

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
// In-memory k-way merge of the accumulated GPU flush segments for thread ti.
// Merges sorted segments directly into a BLF file on the NVMe; dedup integrated.
// Checks drive space after writing and triggers DoIntermediateMerge if low.
// Resets segment tracking so the buffer is ready for the next accumulation round.
// ============================================================================

void FlushMergeWriterBuffer(int ti, PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt  = pCtx->pState;
    int                       level = (int)pSt->playLevel;

    if (pSt->mwSegCount[ti] == 0) return;

    char outPath[MAX_FULL_PATH_NAME];
    snprintf(outPath, sizeof(outPath), "%s\\writer_%04d.bin",
             pSt->mwDirectory[ti], pSt->mwFileCount[ti]++);

    BLFWriter* pw = BLFWriterOpen(outPath);

    std::priority_queue<InMemHead, std::vector<InMemHead>, InMemHeadGreater> heap;

    for (int s = 0; s < pSt->mwSegCount[ti]; s++)
    {
        const BOARD_KEY* pSeg = (const BOARD_KEY*)pSt->pMWBuffer[ti]
                                + pSt->mwSegOffset[ti][s];
        if (pSt->mwSegSize[ti][s] > 0)
            heap.push({ pSeg, pSeg + pSt->mwSegSize[ti][s] });
    }

    BOARD_KEY lastKey = {};
    bool      hasLast = false;

    while (!heap.empty())
    {
        InMemHead top = heap.top();
        heap.pop();

        if (!hasLast || BoardKeyCompare(top.pCurrent, &lastKey) != 0)
        {
            BLFWriterRecord(pw, top.pCurrent);
            lastKey = *top.pCurrent;
            hasLast = true;
        }

        top.pCurrent++;
        if (top.pCurrent < top.pEnd)
            heap.push(top);
    }

    uint64_t unique    = BLFWriterClose(pw);
    uint64_t fileBytes = unique * sizeof(BOARD_KEY) + sizeof(BlasterFileTrailer);

    pSt->levelStats[level].boardsWrittenToDisk += unique;
    pSt->levelStats[level].mwFilesCreated++;
    pSt->levelStats[level].mwBytes += fileBytes;

    // Reset segment tracking for this thread
    pSt->mwSegCount[ti]    = 0;
    pSt->mwBoardsUsed[ti]  = 0;

    // Check drive space; trigger intermediate merge (NVMe → HDD) if low
    char driveRoot[4] = { pSt->mwDirectory[ti][0], ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};
    GetDiskFreeSpaceExA(driveRoot, &freeAvail, nullptr, nullptr);

    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        if (pSt->writerDriveStats[i].driveLetter == pSt->mwDirectory[ti][0])
        {
            pSt->writerDriveStats[i].levelFilesWritten++;
            pSt->writerDriveStats[i].levelBytesWritten += fileBytes;
            pSt->writerDriveStats[i].lastFreeBytes = freeAvail.QuadPart;
            if (freeAvail.QuadPart < pSt->writerDriveStats[i].threshold)
                DoIntermediateMerge(ti, pCtx);
            break;
        }
    }
}

// ============================================================================
// DoIntermediateMerge
// ============================================================================

void DoIntermediateMerge(int mwIdx, PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt    = pCtx->pState;
    const char*               mwDir  = pSt->mwDirectory[mwIdx];
    int                       level  = (int)pSt->playLevel;

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;
    char**    inputPaths     = (char**)MemMalloc("mergeInputPaths",
                                                  (size_t)kMaxInputFiles * sizeof(char*));
    if (!inputPaths)
        Fatal(FATAL_ALLOCATION_FAILED, "DoIntermediateMerge: cannot allocate path array");

    uint64_t totalInputBytes = 0;
    int numFiles = EnumerateWriterFiles(mwDir, inputPaths, kMaxInputFiles, &totalInputBytes);

    if (numFiles == 0)
    {
        MemFree(inputPaths);
        return;
    }

    LoggerLog("IntermediateMerge: mw[%d] %c: -- %d files, %.2f GB\n",
              mwIdx, mwDir[0], numFiles,
              (double)totalInputBytes / (1024.0 * 1024.0 * 1024.0));

    const char* destDir = SelectMergeDestination(pSt, totalInputBytes);
    if (!destDir)
        Fatal(FATAL_DRIVE_SPACE,
              "IntermediateMerge: no merge drive has %.2f GB free (mw[%d] %c:)",
              (double)totalInputBytes / (1024.0 * 1024.0 * 1024.0), mwIdx, mwDir[0]);

    int mergeDirIdx = 0;
    for (int i = 0; i < pSt->numMergeDirs; i++)
        if (pSt->mergeDirectory[i][0] == destDir[0]) { mergeDirIdx = i; break; }

    char outPath[MAX_FULL_PATH_NAME];
    snprintf(outPath, sizeof(outPath), "%s\\imerge_L%03d_%04d.bin",
             destDir, level, pSt->mergeFileCount[mergeDirIdx]++);

    int      tempCount    = 0;
    uint64_t uniqueBoards = CascadingMerge(inputPaths, numFiles,
                                            destDir, outPath, &tempCount, level, nullptr);

    for (int i = 0; i < numFiles; i++)
    {
        DeleteFileA(inputPaths[i]);
        MemFree(inputPaths[i]);
    }
    MemFree(inputPaths);

    pSt->mwFileCount[mwIdx] = 0;

    LoggerLog("IntermediateMerge: → '%s' (%llu unique boards)\n", outPath, uniqueBoards);
}

// ============================================================================
// DoEndOfLevelMerge
// ============================================================================

void DoEndOfLevelMerge(PSolveContext pCtx)
{
    POthelloLevelBlasterState pSt  = pCtx->pState;
    int                       level = (int)pSt->playLevel;

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;
    char**    inputPaths     = (char**)MemMalloc("eolInputPaths",
                                                  (size_t)kMaxInputFiles * sizeof(char*));
    if (!inputPaths)
        Fatal(FATAL_ALLOCATION_FAILED, "DoEndOfLevelMerge: cannot allocate path array");

    int      numFiles        = 0;
    uint64_t totalInputBytes = 0;

    for (int i = 0; i < pSt->numMergeWriters && numFiles < kMaxInputFiles; i++)
    {
        uint64_t dirBytes = 0;
        numFiles += EnumerateWriterFiles(pSt->mwDirectory[i],
                                          inputPaths + numFiles,
                                          kMaxInputFiles - numFiles, &dirBytes);
        totalInputBytes += dirBytes;
    }
    for (int i = 0; i < pSt->numMergeDirs && numFiles < kMaxInputFiles; i++)
    {
        uint64_t dirBytes = 0;
        numFiles += EnumerateWriterFiles(pSt->mergeDirectory[i],
                                          inputPaths + numFiles,
                                          kMaxInputFiles - numFiles, &dirBytes);
        totalInputBytes += dirBytes;
    }

    char outPath[MAX_FULL_PATH_NAME];
    snprintf(outPath, sizeof(outPath), "%s\\Level_%04d_file_0000.bin",
             pSt->storeDirectory, level + 1);

    if (numFiles == 0)
    {
        LoggerLog("EndOfLevelMerge: level %d -- no files to merge\n", level);
        MemFree(inputPaths);
        return;
    }

    pSt->mergeTotalInputBytes = totalInputBytes;
    pSt->mergeProgressBytes   = 0;

    uint64_t uniqueBoards;

    if (numFiles == 1)
    {
        BLFReader* r = BLFOpen(inputPaths[0]);
        uint64_t recordCount = 0;
        if (r)
        {
            const BlasterFileTrailer* tr = BLFTrailer(r);
            if (tr) recordCount = tr->recordCount;
            BLFClose(&r);
        }
        if (!MoveFileExA(inputPaths[0], outPath, MOVEFILE_COPY_ALLOWED))
            Fatal(FATAL_FILE_OPEN,
                  "EndOfLevelMerge: cannot move '%s' → '%s' (err %lu)",
                  inputPaths[0], outPath, GetLastError());
        uniqueBoards = recordCount;
        pSt->mergeProgressBytes = uniqueBoards * sizeof(BOARD_KEY);  // instant for a move
    }
    else
    {
        const char* tempDir = (pSt->numMergeDirs > 0)
                              ? pSt->mergeDirectory[0]
                              : pSt->storeDirectory;
        int tempCount = 0;
        uniqueBoards  = CascadingMerge(inputPaths, numFiles,
                                        tempDir, outPath, &tempCount, level,
                                        &pSt->mergeProgressBytes);
        for (int i = 0; i < numFiles; i++)
            DeleteFileA(inputPaths[i]);
    }

    for (int i = 0; i < numFiles; i++)
        MemFree(inputPaths[i]);
    MemFree(inputPaths);

    uint64_t outBytes = uniqueBoards * sizeof(BOARD_KEY) + sizeof(BlasterFileTrailer);
    pSt->levelStats[level].mrgDupsRemoved =
        pSt->levelStats[level].boardsWrittenToDisk - uniqueBoards;
    pSt->levelStats[level].mergeBytes = outBytes;

}
