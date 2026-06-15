#pragma once
#include <chrono>
#include <time.h>
#include <stdio.h>

// Fills outStr with the local date/time in "YYYY-MM-DD_HH-MM-SS" format.
// Suitable for use in file names (no colons or spaces).
inline void GetCurrentDateTimeString(char* outStr, size_t outSize)
{
    time_t t = time(nullptr);
    struct tm tm = {};
    localtime_s(&tm, &t);
    snprintf(outStr, outSize, "%04d-%02d-%02d_%02d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

typedef struct _ClockTick
{
	std::chrono::time_point<std::chrono::steady_clock> startingTick;
} ClockTick, *PClockTick;

typedef struct _ClockTime
{
	/* The following string represents the current time down to the nanoseconds in the following format */
	/* YYYYMMDDHHMMSSnnnnnnnnn                                                                          */
	char strTime[24];
} ClockTime, *PClockTime;

void ClockStart(PClockTick pClockTick);
long long ClockNanosSinceStart(PClockTick pClockTick);
long long ClockMillisSinceStart(PClockTick pClockTick);
int ClockCompare(PClockTick pC1, PClockTick pC2);
void ClockPrintNanos(FILE* fpOut, PClockTick pClockTick);

void ClockGetSystemClockTime(PClockTime pClockTime);
int ClockCompareSystemClockTime(PClockTime pT1, PClockTime pT2);