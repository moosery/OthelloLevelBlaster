#include "BinarySearch.h"

long long BinarySearch(void *dataArray, void* pKey, long long numElements, long long sizeOfElementInArray, int (*pComp)(void *pContext, const void *pEntry, const void *pKey), void *pContext)
{
	long long leftIdx = 0;
	long long rightIdx = numElements;
	char* pFirstByte = (char*)dataArray;

	while (leftIdx < rightIdx)
	{
		long long midIdx = leftIdx + ((rightIdx - leftIdx) >> 1);
		char *pLocToCompare = pFirstByte + (midIdx * sizeOfElementInArray);
		int cmpVal = pComp(pContext, (void *) pLocToCompare, pKey);

		if (cmpVal < 0)
			leftIdx = midIdx + 1;
		else
			rightIdx = midIdx;
	}

	if (leftIdx < numElements)
	{
		char *pLocToCompare = pFirstByte + (leftIdx * sizeOfElementInArray);
		if (pComp(pContext, (void*)pLocToCompare, pKey) == 0)
			return leftIdx;
	}

	return -(leftIdx + 1);
}
