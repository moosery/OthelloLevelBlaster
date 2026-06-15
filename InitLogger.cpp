#include "InitLogger.h"

void InitLogger(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState)
{
    char dateStr[32];
    GetCurrentDateTimeString(dateStr, sizeof(dateStr));

    if (!CreateFullPath(pConfig->cacheDirName))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create cache directory '%s'", pConfig->cacheDirName);

    snprintf(pState->logFileName, MAX_FULL_PATH_NAME, "%s\\log_%s.txt", pConfig->cacheDirName, dateStr);
    LoggerInit(pState->logFileName);
    LoggerLog("Othello Level Blaster! (Version %s)\n", VERSION);
}
