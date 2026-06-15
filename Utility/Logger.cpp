#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include "Utility.h"

static FILE *g_filePtr = nullptr;
static char g_logFileName[MAX_FULL_PATH_NAME] = { 0 };

void LoggerInit(const char* logFileName)
{
    if (g_filePtr != nullptr)
    {
        fprintf(stderr, "LoggerInit: Logger already initialized with file '%s'; closing previous file\n", g_logFileName);
        fclose(g_filePtr);
        g_filePtr = nullptr;
    }

    if (logFileName != nullptr)
    {
        snprintf(g_logFileName, MAX_FULL_PATH_NAME, "%s", logFileName);
        g_filePtr = _fsopen(g_logFileName, "w", _SH_DENYNO);
        if (g_filePtr == nullptr)
        {
            fprintf(stderr, "LoggerInit: Failed to open log file '%s' for writing\n", g_logFileName);
        }
    }
    else
    {
        fprintf(stderr, "LoggerInit: No log file name provided; logging disabled\n");
    }
}

void LoggerLog(const char* format, ...)
{
    if (g_filePtr != nullptr)
    {
        va_list args, args2;
        va_start(args, format);
        va_copy(args2, args);
        vfprintf(stdout, format, args);
        fflush(stdout);

        if (g_filePtr != nullptr)
        {
            vfprintf(g_filePtr, format, args2);
            fflush(g_filePtr);
        }
        va_end(args2);
        va_end(args);
    }
}
