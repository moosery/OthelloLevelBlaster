#pragma once
#include <shared_mutex>

#define LOCKNAME_SIZE  32

typedef struct _RWLock
{
	char lockName[LOCKNAME_SIZE + 1];
	char _pad[7];                       // explicit padding: lockName[33] ends at byte 32; pRwLock needs 8-byte alignment at byte 40
	std::shared_mutex *pRwLock;
} RWLock, * PRWLock;

void RWLockInit(const char *pszLockName, const char* pszLocation, PRWLock pLock);
void RWLockReadLock(const char* pszLocation, PRWLock pLock);
bool RWLockReadTryLock(const char* pszLocation, PRWLock pLock, int attempts);
void RWLockReadUnlock(const char* pszLocation, PRWLock pLock);
bool RWLockWriteTryLock(const char* pszLocation, PRWLock pLock, int attempts);
void RWLockWriteLock(const char* pszLocation, PRWLock pLock);
void RWLockWriteUnlock(const char* pszLocation, PRWLock pLock);
void RWLockFree(const char* pszLocation, PRWLock pLock);
void RWLockStats();

