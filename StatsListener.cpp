// Include winsock2 before any project headers to prevent winsock1 conflicts
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "StatsListener.h"
#include "DriveLedger.h"
#include "OthelloTypes.h"
#include "Logger.h"
#include <stdio.h>
#include <string.h>

static void FormatDuration(int64_t nanos, char* out, int outSize)
{
    int64_t s = nanos / 1000000000LL;
    snprintf(out, outSize, "%02lld:%02lld:%02lld",
             s / 3600, (s % 3600) / 60, s % 60);
}

static void BuildStatusResponse(PSolveContext pCtx, char* buf, int bufSize)
{
    POthelloLevelBlasterConfig pCfg = pCtx->pConfig;
    POthelloLevelBlasterState  pSt  = pCtx->pState;
    int      curLevel  = (int)pSt->playLevel;
    int      maxLevel  = (int)pCfg->boardSize * (int)pCfg->boardSize - 4;
    char     dur[16];

    int n = 0;
    n += snprintf(buf + n, bufSize - n,
                  "OthelloLevelBlaster v%s  |  Board: %dx%d  |  Levels: 0..%d\n",
                  VERSION, pCfg->boardSize, pCfg->boardSize, maxLevel - 1);
    n += snprintf(buf + n, bufSize - n, "\n");

    // --- Current level (live stats) ---
    const LevelStats* cur = &pSt->levelStats[curLevel];
    bool    curDone      = (cur->totalNanos > 0);
    int64_t elapsedNanos = curDone ? cur->totalNanos
                                   : ClockNanosSinceStart((PClockTick)&cur->startTick);
    uint64_t brdPerSec   = (elapsedNanos > 0)
                           ? (uint64_t)(cur->boardsReadFromStore * 1000000000LL / elapsedNanos) : 0;
    uint64_t nsBrd       = (cur->boardsReadFromStore > 0)
                           ? (uint64_t)(elapsedNanos / (int64_t)cur->boardsReadFromStore) : 0;
    FormatDuration(elapsedNanos, dur, sizeof(dur));

    const char* phase = curDone ? "done"
                               : (pSt->currentPhase ? pSt->currentPhase : "RUNNING");
    n += snprintf(buf + n, bufSize - n,
                  "=== Level %d / %d  [%s]  %s  (%llu brd/s  %llu ns/brd) ===\n",
                  curLevel, maxLevel - 1, phase,
                  dur, (unsigned long long)brdPerSec, (unsigned long long)nsBrd);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards in (store)      : %llu\n",
                  (unsigned long long)cur->boardsReadFromStore);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards generated (GPU) : %llu\n",
                  (unsigned long long)cur->boardsGenerated);
    n += snprintf(buf + n, bufSize - n,
                  "  GPU dups removed       : %llu\n",
                  (unsigned long long)cur->gpuDupsRemoved);
    n += snprintf(buf + n, bufSize - n,
                  "  GPU flushes            : %llu\n",
                  (unsigned long long)cur->gpuFlushes);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards recv'd (GPU)    : %llu\n",
                  (unsigned long long)cur->boardsReceivedFromGpu);
    n += snprintf(buf + n, bufSize - n,
                  "  Merge dups removed     : %llu\n",
                  (unsigned long long)cur->mrgDupsRemoved);
    n += snprintf(buf + n, bufSize - n,
                  "  MW files created       : %llu\n",
                  (unsigned long long)cur->mwFilesCreated);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards written to disk : %llu  (%.2f GB)\n",
                  (unsigned long long)cur->boardsWrittenToDisk,
                  cur->mwBytes / (1024.0 * 1024.0 * 1024.0));
    if (cur->passBoards > 0 || cur->terminalBoards > 0)
    {
        n += snprintf(buf + n, bufSize - n,
                      "  Pass boards            : %llu\n",
                      (unsigned long long)cur->passBoards);
        n += snprintf(buf + n, bufSize - n,
                      "  Terminal boards        : %llu\n",
                      (unsigned long long)cur->terminalBoards);
    }
    n += snprintf(buf + n, bufSize - n, "\n");

    // Current-level drive breakdown (cumulative since level start)
    n += snprintf(buf + n, bufSize - n,
                  "  Drv  Dirs  Files    Disk GB   Uncomp GB     Free GB  Blk  Wht\n");
    n += snprintf(buf + n, bufSize - n,
                  "  ---  ----  -----  ---------  ----------  ----------  ---  ---\n");
    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        const WriterDriveStats* d = &pSt->writerDriveStats[i];

        // Sum live writer file counts across threads on this drive
        int liveBlack = 0, liveWhite = 0;
        for (int ti = 0; ti < pSt->numMergeWriters; ti++)
        {
            if (pSt->mwDirectory[ti][0] == d->driveLetter)
            {
                liveBlack += pSt->mwBlackFileCount[ti];
                liveWhite += pSt->mwWhiteFileCount[ti];
            }
        }

        bool showUncomp = (d->levelBytesUncompressed > 0
                           && d->levelBytesUncompressed != d->levelBytesWritten);
        if (showUncomp)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5llu  %6.2f GB  %7.2f GB  %7.2f GB  %3d  %3d\n",
                          d->driveLetter, d->numDirs,
                          (unsigned long long)d->levelFilesWritten,
                          d->levelBytesWritten      / (1024.0 * 1024.0 * 1024.0),
                          d->levelBytesUncompressed / (1024.0 * 1024.0 * 1024.0),
                          DriveAvailable(pSt, d->driveLetter) / (1024.0 * 1024.0 * 1024.0),
                          liveBlack, liveWhite);
        else
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5llu  %6.2f GB            %7.2f GB  %3d  %3d\n",
                          d->driveLetter, d->numDirs,
                          (unsigned long long)d->levelFilesWritten,
                          d->levelBytesWritten / (1024.0 * 1024.0 * 1024.0),
                          DriveAvailable(pSt, d->driveLetter) / (1024.0 * 1024.0 * 1024.0),
                          liveBlack, liveWhite);
    }

    // Merge dir free space (F:, etc.) — read from ledger, no OS query
    for (int i = 0; i < pSt->numMergeDirs; i++)
    {
        n += snprintf(buf + n, bufSize - n,
                      "  Merge drv  %c:  free = %.2f GB\n",
                      pSt->mergeDirectory[i][0],
                      DriveAvailable(pSt, pSt->mergeDirectory[i][0])
                          / (1024.0 * 1024.0 * 1024.0));
    }

    // Active intermediate merges (per writer thread)
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        if (pSt->imergeActive[i])
        {
            double doneGB  = pSt->imergeDoneInputBytes[i]  / (1024.0 * 1024.0 * 1024.0);
            double totalGB = pSt->imergeTotalInputBytes[i] / (1024.0 * 1024.0 * 1024.0);
            double pct     = (pSt->imergeTotalInputBytes[i] > 0)
                             ? 100.0 * (double)pSt->imergeDoneInputBytes[i]
                                     / (double)pSt->imergeTotalInputBytes[i]
                             : 0.0;
            n += snprintf(buf + n, bufSize - n,
                          "  iMerge mw[%d] %c:       : %.2f / %.2f GB  (%.1f%%)\n",
                          i, pSt->mwDirectory[i][0], doneGB, totalGB, pct);
        }
    }

    // Cascade progress — shown when CascadingMerge is writing intermediate temp files.
    // That player's merge % (in the history row) will read 0 until the final pass starts;
    // these lines give visibility into what would otherwise look like a stalled merge.
    for (int p = 0; p <= 1; p++)
    {
        if (pSt->cascadeActive[p])
        {
            const char* playerName = (p == 1) ? "black" : "white";
            double gbWritten = (double)(int64_t)pSt->cascadeGroupProgressBytes[p]
                               / (1024.0 * 1024.0 * 1024.0);
            n += snprintf(buf + n, bufSize - n,
                          "  Cascade %-5s         : group %d / %d  (%.2f GB to temp)\n",
                          playerName,
                          pSt->cascadeGroupsDone[p] + 1,
                          pSt->cascadeNumGroups[p],
                          gbWritten);
        }
    }

    // Store drive free space — read from ledger, no OS query
    n += snprintf(buf + n, bufSize - n,
                  "  Store drv  %c:  free = %.2f TB\n",
                  pCfg->storeDrive,
                  DriveAvailable(pSt, pCfg->storeDrive)
                      / (1024.0 * 1024.0 * 1024.0 * 1024.0));

    // --- Level history table (completed levels + current in-progress row) ---
    n += snprintf(buf + n, bufSize - n, "\n");
    n += snprintf(buf + n, bufSize - n,
                  "Lvl        BoardsIn        Generated         GpuDups         MrgDups         Written       SlvGB  Duration      ns/brd\n");
    n += snprintf(buf + n, bufSize - n,
                  "---  --------------  ---------------  --------------  --------------  --------------  ----------  --------  ----------\n");
    for (int lvl = pSt->resumeLevel; lvl < curLevel; lvl++)
    {
        const LevelStats* ls = &pSt->levelStats[lvl];
        FormatDuration(ls->totalNanos, dur, sizeof(dur));
        uint64_t ns = (ls->boardsReadFromStore > 0)
                      ? (uint64_t)(ls->totalNanos / (int64_t)ls->boardsReadFromStore) : 0;
        n += snprintf(buf + n, bufSize - n,
                      "%3d  %14llu  %15llu  %14llu  %14llu  %14llu  %10.2f  %8s  %10llu\n",
                      lvl,
                      (unsigned long long)ls->boardsReadFromStore,
                      (unsigned long long)ls->boardsGenerated,
                      (unsigned long long)ls->gpuDupsRemoved,
                      (unsigned long long)ls->mrgDupsRemoved,
                      (unsigned long long)ls->boardsWrittenToDisk,
                      ls->mwBytes / (1024.0 * 1024.0 * 1024.0),
                      dur,
                      (unsigned long long)ns);
    }
    // Current level row with live partial data and phase tag
    {
        char curDur[16];
        FormatDuration(elapsedNanos, curDur, sizeof(curDur));
        char phaseStr[24] = "[running]";
        if (curDone)
            snprintf(phaseStr, sizeof(phaseStr), "[done]");
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "Merging to store") == 0)
        {
            double wPct = (pSt->mergeTotalInputBytes[0] > 0)
                ? 100.0 * (double)pSt->mergeProgressBytes[0] / (double)pSt->mergeTotalInputBytes[0] : 0.0;
            double bPct = (pSt->mergeTotalInputBytes[1] > 0)
                ? 100.0 * (double)pSt->mergeProgressBytes[1] / (double)pSt->mergeTotalInputBytes[1] : 0.0;
            snprintf(phaseStr, sizeof(phaseStr), "[W:%7.3f%%/B:%7.3f%%]", wPct, bPct);
        }
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "Flushing buffers") == 0)
            snprintf(phaseStr, sizeof(phaseStr), "[flushing]");
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "GPU solving") == 0
                 && pSt->currentLevelTotalBoards > 0)
            snprintf(phaseStr, sizeof(phaseStr), "[solve%6.1f%%]",
                     100.0 * (double)cur->boardsReadFromStore
                           / (double)pSt->currentLevelTotalBoards);
        n += snprintf(buf + n, bufSize - n,
                      "%3d  %14llu  %15llu  %14llu  %14llu  %14llu  %10.2f  %8s  %s\n",
                      curLevel,
                      (unsigned long long)cur->boardsReadFromStore,
                      (unsigned long long)cur->boardsGenerated,
                      (unsigned long long)cur->gpuDupsRemoved,
                      (unsigned long long)cur->mrgDupsRemoved,
                      (unsigned long long)cur->boardsWrittenToDisk,
                      cur->mwBytes / (1024.0 * 1024.0 * 1024.0),
                      curDur,
                      phaseStr);
    }

    n += snprintf(buf + n, bufSize - n, "END\n");
    (void)n;
}

static void HandleClient(SOCKET client, PSolveContext pCtx)
{
    char cmd[64] = {};
    int got = recv(client, cmd, (int)sizeof(cmd) - 1, 0);
    if (got <= 0) { closesocket(client); return; }

    // Trim trailing whitespace/newlines
    for (int i = got - 1; i >= 0 && (cmd[i] == '\r' || cmd[i] == '\n' || cmd[i] == ' '); i--)
        cmd[i] = '\0';

    if (_stricmp(cmd, "STOP") == 0)
    {
        const char* msg = "Stopping...\n";
        send(client, msg, (int)strlen(msg), 0);
        LoggerLog("STOP command received via stats port - requesting graceful shutdown...\n");
        pCtx->pState->terminateThreads = true;
    }
    else
    {
        char buf[16384];
        BuildStatusResponse(pCtx, buf, sizeof(buf));
        send(client, buf, (int)strlen(buf), 0);
    }

    closesocket(client);
}

static void RunStatsListenerJob(uint32_t /*thdIdx*/, PSolveContext pCtx)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { WSACleanup(); return; }

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(pCtx->pConfig->statsPort);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listenSock, 4) != 0)
    {
        LoggerLog("Stats listener: failed to bind/listen on port %d\n",
                  (int)pCtx->pConfig->statsPort);
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    LoggerLog("Stats listener running on port %d\n", (int)pCtx->pConfig->statsPort);

    while (!pCtx->pState->terminateStatsListener)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval tv = { 0, 50000 };  // 50 ms

        if (select(0, &readSet, nullptr, nullptr, &tv) <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client != INVALID_SOCKET)
            HandleClient(client, pCtx);
    }

    closesocket(listenSock);
    WSACleanup();
    LoggerLog("Stats listener stopped.\n");
}

void SubmitStatsListenerJob(PSolveContext pCtx)
{
    pCtx->pState->pStatsThreadPool->QueueJob(
        [pCtx](uint32_t thdIdx)
        {
            RunStatsListenerJob(thdIdx, pCtx);
        }
    );
}
