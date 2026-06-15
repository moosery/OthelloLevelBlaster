#include "RWLock.h"
#include <chrono>
#include <thread>
using namespace std;
thread_local std::hash<std::thread::id> theHash;
thread_local size_t myThreadId = theHash(std::this_thread::get_id());

thread_local unsigned long long mutexReadLocked = 0;
thread_local unsigned long long mutexWriteLocked = 0;
thread_local unsigned long long maxReadLocked = 0;
thread_local unsigned long long maxWriteLocked = 0;


//#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG_INSERT		0x0000000000000001
#define DEBUG_INSERT_PATH	0x0000000000000002
#define DEBUG_DELETE        0x0000000000000004
#define DEBUG_LOCKS         0x0000000000000008
#define DEBUG_SEARCH        0x0000000000000010
size_t debugWhat = DEBUG_LOCKS;
FILE* fpDebug = fopen("D:\\DebugRW.txt", "w");
/* Guard struct: its destructor runs at program exit and closes fpDebug,
   since there is no explicit shutdown function for this module. */
static struct DebugFileGuard
{
    ~DebugFileGuard() { if (fpDebug) { fclose(fpDebug); fpDebug = NULL; } }
} debugFileGuard;
#define IsDebugging(flag)   ((flag) & debugWhat)
#endif

//#define DO_STATS
#ifdef DO_STATS
typedef struct _LocksHeld
{
	PRWLock pLock;
	char lockName[LOCKNAME_SIZE + 1];
} LOCKSHELD, *PLOCKSHELD;

#define MAXLOCKS 1000
thread_local LOCKSHELD activeReadsHeld[MAXLOCKS];
thread_local LOCKSHELD activeWritesHeld[MAXLOCKS];
thread_local LOCKSHELD maxWritesHeld[MAXLOCKS];
#endif

#ifdef DO_STATS
void removeFromArray(PRWLock pLock, unsigned long long numHeld, LOCKSHELD locksHeld[])
{
	bool found = false;

	for(unsigned long long idx = 0; idx < numHeld; idx++)
	{
		if (strncmp(locksHeld[idx].lockName, pLock->lockName,LOCKNAME_SIZE) == 0 && pLock == locksHeld[idx].pLock)
		{
			found = true;
			memcpy(&(locksHeld[idx]), &(locksHeld[idx + 1]), (numHeld - idx - 1) * sizeof(LOCKSHELD));
		}
	}

	if (!found)
	{
		fprintf(stderr, "Trying to unlock a lock that isn't held! '%s' \n", pLock->lockName);
		fflush(stderr);
		int x = 1;
		x--;
		x = x / x;
	}
}

void printLocks(FILE* fpOut, const char *pszWhat, unsigned long long numHeld, LOCKSHELD locksHeld[])
{
	fprintf(fpOut, "%s\n", pszWhat);
	for (size_t x = 0; x < numHeld; x++)
	{
		fprintf(fpOut, "%s: 0x%016p\n", locksHeld[x].lockName, locksHeld[x].pLock);
	}
	fprintf(fpOut, "====================\n");
}
#endif

void RWLockFree(const char* pszLocation, PRWLock pLock)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockFree Start\n", myThreadId,clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif
	delete pLock->pRwLock;
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockFree End\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif
}

void RWLockInit(const char *pszLockName, const char *pszLocation, PRWLock pLock)
{
	strncpy(pLock->lockName, pszLockName,LOCKNAME_SIZE);
	pLock->lockName[LOCKNAME_SIZE] = '\0';
	pLock->_pad[0] = pLock->_pad[1] = pLock->_pad[2] = pLock->_pad[3] =
	pLock->_pad[4] = pLock->_pad[5] = pLock->_pad[6] = 0;
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockInit Start\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif

	pLock->pRwLock = new std::shared_mutex;

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockInit End\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif
}

void RWLockReadLock(const char* pszLocation, PRWLock pLock)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadLock Waiting\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif

	pLock->pRwLock->lock_shared();
#ifdef DO_STATS
	strncpy(activeReadsHeld[mutexReadLocked].lockName, pLock->lockName, LOCKNAME_SIZE);
	activeReadsHeld[mutexReadLocked].lockName[LOCKNAME_SIZE] = '\0';
	activeReadsHeld[mutexReadLocked].pLock = pLock;
#endif

#if defined(DEBUG_ON) || defined(DO_STATS)
	mutexReadLocked++;
	if (mutexReadLocked > maxReadLocked)
		maxReadLocked = mutexReadLocked;

#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadLock Obtained\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
		printLocks(fpDebug, "Read Locks", mutexReadLocked, activeReadsHeld);
	}
#endif

}
bool RWLockReadTryLock(const char* pszLocation, PRWLock pLock, int attempts)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadLock Trying for %d attempts\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation, attempts);
		fflush(fpDebug);
	}
#endif

	bool result = false;

	while (!result && attempts > 0)
	{
		result = pLock->pRwLock->try_lock_shared();
		if (!result)
		{
			std::this_thread::sleep_for(100ms);
			attempts--;
		}
	}

#ifdef DO_STATS
	if (result)
	{
		strncpy(activeReadsHeld[mutexReadLocked].lockName, pLock->lockName, LOCKNAME_SIZE);
		activeReadsHeld[mutexReadLocked].lockName[LOCKNAME_SIZE] = '\0';
		activeReadsHeld[mutexReadLocked].pLock = pLock;
	}
#endif

#if defined(DEBUG_ON) || defined(DO_STATS)
	if (result)
	{
		mutexReadLocked++;
		if (mutexReadLocked > maxReadLocked)
			maxReadLocked = mutexReadLocked;
	}
#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		if (result)
		{
			fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadLock Obtained\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
			printLocks(fpDebug, "Read Locks", mutexReadLocked, activeReadsHeld);
		}
		else
			fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadLock Failed\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);

		fflush(fpDebug);
	}
#endif

	return result;
}

void RWLockReadUnlock(const char* pszLocation, PRWLock pLock)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%16zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadUnlock Release Start\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		printLocks(fpDebug, "Read Locks", mutexReadLocked, activeReadsHeld);
		fflush(fpDebug);
	}
#endif
	pLock->pRwLock->unlock_shared();

#if defined(DEBUG_ON) || defined(DO_STATS)
	removeFromArray(pLock, mutexReadLocked, activeReadsHeld);
	if (mutexReadLocked > 0)
	{
		mutexReadLocked--;
	}
	else
	{
		fprintf(stderr, "Trying to free a read lock that you do not have!!!!!\n");
	}

#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockReadUnlock Release Complete\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		printLocks(fpDebug, "Read Locks", mutexReadLocked, activeReadsHeld);
		fflush(fpDebug);
	}
#endif

}

void RWLockWriteLock(const char* pszLocation, PRWLock pLock)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteLock Waiting\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		fflush(fpDebug);
	}
#endif
	pLock->pRwLock->lock();

#ifdef DO_STATS
	strncpy(activeWritesHeld[mutexWriteLocked].lockName, pLock->lockName, LOCKNAME_SIZE);
	activeWritesHeld[mutexWriteLocked].lockName[LOCKNAME_SIZE] = '\0';
	activeWritesHeld[mutexWriteLocked].pLock = pLock;
#endif

#if defined(DEBUG_ON) || defined(DO_STATS)
	mutexWriteLocked++;
#endif

#ifdef DO_STATS
	if (mutexWriteLocked > maxWriteLocked)
	{
		for (maxWriteLocked = 0; maxWriteLocked < mutexWriteLocked; maxWriteLocked++)
		{
			strcpy(maxWritesHeld[maxWriteLocked].lockName, activeWritesHeld[maxWriteLocked].lockName);
			maxWritesHeld[maxWriteLocked].pLock = activeWritesHeld[maxWriteLocked].pLock;
		}
	}
#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteLock Obtained\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		printLocks(fpDebug, "Write Locks", mutexWriteLocked, activeWritesHeld);
		fflush(fpDebug);
	}
#endif

}

bool RWLockWriteTryLock(const char* pszLocation, PRWLock pLock, int attempts)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteLock Trying for %d attempts\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation, attempts);
		fflush(fpDebug);
	}
#endif
	bool result = false;

	while (!result && attempts > 0)
	{
		result = pLock->pRwLock->try_lock();
		if (!result)
		{
			std::this_thread::sleep_for(100ms);
			attempts--;
		}
	}
#ifdef DO_STATS
	if (result)
	{
		strcpy(activeWritesHeld[mutexWriteLocked].lockName, pLock->lockName);
		activeWritesHeld[mutexWriteLocked].pLock = pLock;
	}
#endif

#if defined(DEBUG_ON) || defined(DO_STATS)
	if(result)
		mutexWriteLocked++;
#endif

#ifdef DO_STATS
	if (result && mutexWriteLocked > maxWriteLocked)
	{
		for (maxWriteLocked = 0; maxWriteLocked < mutexWriteLocked; maxWriteLocked++)
		{
			strcpy(maxWritesHeld[maxWriteLocked].lockName, activeWritesHeld[maxWriteLocked].lockName);
			maxWritesHeld[maxWriteLocked].pLock = activeWritesHeld[maxWriteLocked].pLock;
		}
	}
#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		if (result)
		{
			fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteLock Obtained\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
			printLocks(fpDebug, "Write Locks", mutexWriteLocked, activeWritesHeld);
		}
		else
			fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteLock Failed\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);

		fflush(fpDebug);
	}
#endif

	return result;
}

void RWLockWriteUnlock(const char* pszLocation, PRWLock pLock)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteUnlock Release Start\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		printLocks(fpDebug, "Write Locks", mutexWriteLocked, activeWritesHeld);
		fflush(fpDebug);
	}
#endif

	pLock->pRwLock->unlock();

#if  defined(DO_STATS) || defined(DEBUG_ON)
	removeFromArray(pLock, mutexWriteLocked, activeWritesHeld);
	if (mutexWriteLocked > 0)
	{
		mutexWriteLocked--;
	}
	else
	{
		fprintf(stderr, "Trying to free a write lock that you do not have!!!!!\n");
	}
#endif

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Clock: 0x%08lx  RWLock:0x%p - %s - %s: RWLockWriteUnlock Release Complete\n", myThreadId, clock(), pLock, pLock->lockName, pszLocation);
		printLocks(fpDebug, "Write Locks", mutexWriteLocked, activeWritesHeld);
		fflush(fpDebug);
	}
#endif


}

void RWLockStats()
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_LOCKS))
	{
		fprintf(fpDebug, "0x%016zx: Number of read  locks: %zd\n",myThreadId, mutexReadLocked);
		fprintf(fpDebug, "0x%016zx: Number of write locks: %zd\n", myThreadId, mutexWriteLocked);
		fflush(fpDebug);
	}
#endif

#ifdef DO_STATS
	FILE *fpOut = stdout;
#ifdef DEBUG_ON
	fpOut = fpDebug;
#endif
	fprintf(fpOut, "0x%016zx: Number of read  locks: %zd\n", myThreadId, mutexReadLocked);
	fprintf(fpOut, "0x%016zx: Number of write locks: %zd\n", myThreadId, mutexWriteLocked);
	fprintf(fpOut, "0x%016zx: Max Number of read  locks: %zd\n", myThreadId, maxReadLocked);
	fprintf(fpOut, "0x%016zx: Max Number of write locks: %zd\n", myThreadId, maxWriteLocked);
	fprintf(fpOut, "========================================\n");

	printLocks(fpOut, "Reads Held:", mutexReadLocked, activeReadsHeld);

	printLocks(fpOut, "Writes Held:", mutexWriteLocked, activeWritesHeld);

	printLocks(fpOut, "Max Writes Held", maxWriteLocked, maxWritesHeld);

	fflush(fpOut);

#endif
}
