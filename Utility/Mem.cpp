#include <stdio.h>
#include <memory.h>
#include <malloc.h>
#include <string.h>
#include <shared_mutex>
#include <windows.h>
#include <memoryapi.h>

using namespace std;

#define NOTRACK 1
//#define MEMDEBUG 1

#define NAMESIZE 31

typedef struct _Memory_Node
{
	char controlStr[8];
	char memName[NAMESIZE + 1];
	size_t memSize;
	struct _Memory_Node* pNextNode;
	struct _Memory_Node* pPrevNode;
}MEMORY_NODE, * PMEMORY_NODE;

PMEMORY_NODE pFirstNode = NULL;
PMEMORY_NODE pLastNode = NULL;
#define CONTROLSTR "JWS1234"
const char THE_MEMORY_OVERWRITE_CHECK_STR[52] = "Now is the time to see if the data got overwritten!";

typedef shared_mutex MyMallocLock;
typedef unique_lock<MyMallocLock> MyMallocWriteLock;
typedef shared_lock<MyMallocLock> MyMallocReadLock;

MyMallocLock myMallocLock;
size_t totalAllocated = 0;

typedef struct _statsInfo
{
	char name[NAMESIZE + 1];
	size_t numAllocated;
}STATSINFO, * PSTATSINFO;


void MemStatsPrint(FILE *fpOut)
{
#ifdef NOTRACK
	printf("Memory is not being tracked!\n");
#else
	STATSINFO theStats[32];

	memset(theStats, 0, sizeof(theStats));

	MyMallocReadLock w_lock(myMallocLock);
	{
		PMEMORY_NODE pNode = pFirstNode;

		while (pNode != NULL)
		{
//#define SUPERDETAIL
#ifdef SUPERDETAIL
			printf("Address: 0x%p  User Address: 0x%p  Name: %s\n", pNode, &(pNode[1]),pNode->memName);
#endif
			int idx = 0;
			for (idx = 0; idx < 32; idx++)
			{
				if (theStats[idx].name[0] == '\0')
					break;
				if (strcmp(theStats[idx].name, pNode->memName) == 0)
					break;
			}

			if (idx < 32)
			{
				if (theStats[idx].name[0] == '\0')
					memcpy(theStats[idx].name, pNode->memName, sizeof(theStats[idx].name));

				(theStats[idx].numAllocated)++;
			}
			pNode = pNode->pNextNode;
		}
	}

	fprintf(fpOut,"Memory Allocated:\n");
	for (int idx = 0; idx < 32; idx++)
	{
		if (theStats[idx].name[0] == '\0')
			break;

		fprintf(fpOut,"  '%s': %llu\n", theStats[idx].name, theStats[idx].numAllocated);
	}

	fprintf(fpOut,"Total Size Allocated: %llu\n", totalAllocated);

#endif
}

void* MemMalloc(const char* pStr, size_t sizeToAlloc)
{
#ifdef NOTRACK
	void* result = (void*)malloc(sizeToAlloc);

	if (result != NULL)
		memset(result, 0, sizeToAlloc);

	return result;
#else
	PMEMORY_NODE pNewNode;
	size_t largerSize = sizeToAlloc + sizeof(MEMORY_NODE) + sizeof(THE_MEMORY_OVERWRITE_CHECK_STR);

	pNewNode = (PMEMORY_NODE)malloc(largerSize);

	if (pNewNode == NULL)
	{
		fprintf(stderr, "Could not malloc size of %llu in MemMalloc for structure %s\n", largerSize, pStr);
		return NULL;
	}

#ifdef MEMDEBUG
	printf("Allocated '0x%08p': '%s' for a size of %llu (%llu with overhead)\n", pNewNode, pStr, sizeToAlloc, largerSize);
#endif
	memset(pNewNode, 0, largerSize);
	memcpy(pNewNode->controlStr, CONTROLSTR, 8);

	strncpy(pNewNode->memName, pStr, NAMESIZE);
	pNewNode->memSize = sizeToAlloc;

	char* pOverWriteString = ((char*)pNewNode) + sizeToAlloc + sizeof(MEMORY_NODE);
	memcpy(pOverWriteString, THE_MEMORY_OVERWRITE_CHECK_STR, sizeof(THE_MEMORY_OVERWRITE_CHECK_STR));


	{
		MyMallocWriteLock w_lock(myMallocLock);

		if (pFirstNode == NULL)
		{
			pFirstNode = pNewNode;
			pLastNode = pNewNode;
		}
		else
		{
			pNewNode->pPrevNode = pLastNode;
			pLastNode->pNextNode = pNewNode;
			pLastNode = pNewNode;
		}

		totalAllocated += sizeToAlloc;
	}


	return(&(pNewNode[1]));
#endif
}

void MemCheck(FILE *fpOut, const char *pszStr)
{
#ifdef NOTRACK
	printf("Memory is not being tracked!\n");
#else
	MyMallocReadLock w_lock(myMallocLock);
	{
		PMEMORY_NODE pNode = pFirstNode;

		while (pNode != NULL)
		{
			if (strcmp(pNode->controlStr, CONTROLSTR) != 0)
			{
				fprintf(fpOut, "There is an invalid control string in a memory tracking node (%s)!\n",pszStr);
				return;
			}

			if (memcmp(THE_MEMORY_OVERWRITE_CHECK_STR, ((char*)pNode) + pNode->memSize + sizeof(MEMORY_NODE), sizeof(THE_MEMORY_OVERWRITE_CHECK_STR)) != 0)
			{
				fprintf(fpOut, "The memory node for '%s' has been the victim of an overrun (%s)!\n", pNode->memName,pszStr);
				return;
			}

			pNode = pNode->pNextNode;
		}
	}


#endif

}

void MemFree(void* pPtr)
{
	if (pPtr == NULL)
		return;
#ifdef NOTRACK
	free(pPtr);
#else
	PMEMORY_NODE pTmp = (PMEMORY_NODE)pPtr;
	pTmp--;

	if (strcmp(pTmp->controlStr, CONTROLSTR) != 0)
	{
		fprintf(stderr, "Attempt to MemFree a buffer not allocated by MemMalloc!\n");
		return;
	}
#ifdef MEMDEBUG
	printf("Freeing '0x%08p': '%s' size of %llu\n", pTmp, pTmp->memName, pTmp->memSize);
#endif

	/* Now do a check for any overwrite */
	char* pOverWriteString = ((char*)pTmp) + pTmp->memSize + sizeof(MEMORY_NODE);
	if (memcmp(pOverWriteString, THE_MEMORY_OVERWRITE_CHECK_STR, sizeof(THE_MEMORY_OVERWRITE_CHECK_STR)) != 0)
	{
		fprintf(stderr, "The memory block has been corrupted (written past end of buffer)!!\n");
		return;
	}
	{
		MyMallocWriteLock w_lock(myMallocLock);
		if (pTmp == pFirstNode)
		{
			if (pTmp == pLastNode)
			{
				pFirstNode = NULL;
				pLastNode = NULL;
			}
			else
			{
				pFirstNode = pFirstNode->pNextNode;
				pFirstNode->pPrevNode = NULL;
			}
		}
		else if (pTmp == pLastNode)
		{
			pLastNode = pLastNode->pPrevNode;
			pLastNode->pNextNode = NULL;
		}
		else
		{
			if (pTmp->pNextNode != NULL)
				pTmp->pNextNode->pPrevNode = pTmp->pPrevNode;

			if (pTmp->pPrevNode != NULL)
				pTmp->pPrevNode->pNextNode = pTmp->pNextNode;
		}
		totalAllocated -= pTmp->memSize;
	}

	free(pTmp);
#endif
}

size_t MemSize()
{
	return totalAllocated;
}
