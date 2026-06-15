// Include winsock2 before any project headers to prevent winsock1 conflicts
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "StatsListener.h"
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

    // Current-level drive breakdown
    n += snprintf(buf + n, bufSize - n, "  Drv  Dirs  Files   Written      Free\n");
    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        const WriterDriveStats* d = &pSt->writerDriveStats[i];
        n += snprintf(buf + n, bufSize - n,
                      "   %c    %2d  %5llu  %7.2f GB  %6.2f GB\n",
                      d->driveLetter, d->numDirs,
                      (unsigned long long)d->levelFilesWritten,
                      d->levelBytesWritten / (1024.0 * 1024.0 * 1024.0),
                      d->lastFreeBytes     / (1024.0 * 1024.0 * 1024.0));
    }

    // Merge progress (only meaningful during end-of-level merge)
    if (pSt->currentPhase && strcmp(pSt->currentPhase, "Merging to store") == 0
        && pSt->mergeTotalInputBytes > 0)
    {
        double doneGB  = pSt->mergeProgressBytes  / (1024.0 * 1024.0 * 1024.0);
        double totalGB = pSt->mergeTotalInputBytes / (1024.0 * 1024.0 * 1024.0);
        double pct     = 100.0 * (double)pSt->mergeProgressBytes / (double)pSt->mergeTotalInputBytes;
        n += snprintf(buf + n, bufSize - n,
                      "  Merge progress         : %.2f / %.2f GB  (%.1f%%)\n",
                      doneGB, totalGB, pct);
    }

    // Store drive free space (queried live; not in writerDriveStats since it never gets writer dirs)
    {
        char storeRoot[4] = { pCfg->storeDrive, ':', '\\', '\0' };
        ULARGE_INTEGER freeBytes = {};
        GetDiskFreeSpaceExA(storeRoot, &freeBytes, nullptr, nullptr);
        n += snprintf(buf + n, bufSize - n,
                      "  Store drv  %c:  free = %.2f TB\n",
                      pCfg->storeDrive,
                      freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }

    // --- Completed level history (compact table) ---
    if (curLevel > 0)
    {
        n += snprintf(buf + n, bufSize - n, "\n");
        n += snprintf(buf + n, bufSize - n,
                      "Lvl  BoardsIn      Generated    GpuDups    MrgDups    Written       SlvGB    Duration   ns/brd\n");
        n += snprintf(buf + n, bufSize - n,
                      "---  ------------  -----------  ---------  ---------  ------------  -------  ---------  ------\n");
        for (int lvl = 0; lvl < curLevel; lvl++)
        {
            const LevelStats* ls = &pSt->levelStats[lvl];
            FormatDuration(ls->totalNanos, dur, sizeof(dur));
            uint64_t ns = (ls->boardsReadFromStore > 0)
                          ? (uint64_t)(ls->totalNanos / (int64_t)ls->boardsReadFromStore) : 0;
            n += snprintf(buf + n, bufSize - n,
                          "%3d  %12llu  %11llu  %9llu  %9llu  %12llu  %7.2f  %9s  %6llu\n",
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
