#include <stdarg.h>
#include "Error.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_ERROR_LEN 4095
thread_local RC lastError = RC_SUCCESS;
thread_local char errorReason[MAX_ERROR_LEN + 1];

void Error(RC error, const char* pszReasonFmt, ...)
{
	lastError = error;
	va_list argptr;
	va_start(argptr, pszReasonFmt);
	vsnprintf(errorReason, sizeof(errorReason), pszReasonFmt, argptr);
	va_end(argptr);
}

RC ErrorGetLast()
{
	return lastError;
}

char* ErrorGetLastReason()
{
	return errorReason;
}

void ErrorPrint(FILE* fpOut)
{
	fprintf(fpOut, "(%zu): %s\n", lastError, errorReason);
}

void Fatal(RC rc, const char* pszReasonFmt, ...)
{
	va_list argptr;
	va_start(argptr, pszReasonFmt);
	vfprintf(stderr, pszReasonFmt, argptr);
	va_end(argptr);
	fflush(stderr);
	exit((int)rc);
}