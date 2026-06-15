#include "ClockTick.h"
using namespace std;
#include <string.h>

static chrono::steady_clock theClock;

void ClockGetSystemClockTime(PClockTime pClockTime)
{
	auto now = std::chrono::system_clock::now();
	time_t theTime = std::chrono::system_clock::to_time_t(now);
	std::tm* theLocalTime = std::localtime(&theTime);
	std::strftime(pClockTime->strTime, sizeof(pClockTime->strTime), "%Y%m%d%H%M%S", theLocalTime);
	auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() % 1000000000;
	snprintf(pClockTime->strTime + 14, sizeof(pClockTime->strTime) - 14, "%09lld", nanoseconds);
}

int ClockCompareSystemClockTime(PClockTime pT1, PClockTime pT2)
{
	return strcmp(pT1->strTime, pT2->strTime);
}

void ClockStart(PClockTick pClockTick)
{
	pClockTick->startingTick = theClock.now();
}

long long ClockNanosSinceStart(PClockTick pClockTick)
{
	chrono::time_point<chrono::steady_clock> endingTick = theClock.now();

	chrono::duration<long long, std::nano> elapsed = (endingTick - pClockTick->startingTick);

	return (elapsed.count());
}

long long ClockMillisSinceStart(PClockTick pClockTick)
{
	chrono::time_point<chrono::steady_clock> endingTick = theClock.now();

	chrono::duration<double, std::milli> elapsed = (endingTick - pClockTick->startingTick);

	return ((long long) elapsed.count());
}

int ClockCompare(PClockTick pC1, PClockTick pC2)
{
	if (pC1->startingTick > pC2->startingTick)
		return 1;
	else if (pC1->startingTick < pC2->startingTick)
		return -1;
	else
		return 0;
}

void ClockPrintNanos(FILE* fpOut, PClockTick pClockTick)
{
	fprintf(fpOut, "The ClockTick Nanos are: %zd\n", pClockTick->startingTick.time_since_epoch().count());
}