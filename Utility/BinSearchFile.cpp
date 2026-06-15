#include <stdio.h>
#include "Error.h"

long long BinarySearchFile(FILE *fpOut, void* pKey, void *pDataBuffer, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext)
{
	long long leftIdx = 0;
	long long rightIdx = numElements - 1;
	long long midIdx = 0;
	long long seekOffset;


	while (leftIdx <= rightIdx)
	{
		midIdx = leftIdx + ((rightIdx - leftIdx) >> 1);

		seekOffset = (midIdx * sizeOfElementInArray);

		if (_fseeki64(fpOut, seekOffset, SEEK_SET) != 0)
		{
			Fatal(FATAL_SEEK_FAILED, "Could not seek to value of %zd\n", seekOffset);
		}

		if (fread(pDataBuffer, sizeOfElementInArray, 1, fpOut) != 1)
		{
			Fatal(FATAL_READ_FAILED, "Could not read record at seek value of %zd\n", seekOffset);
		}

		int cmpVal = pComp(pContext, (void*)pDataBuffer, pKey);

		if (cmpVal < 0)
		{
			leftIdx = midIdx + 1;
		}
		else if (cmpVal > 0)
		{
			rightIdx = midIdx - 1;
		}
		else
		{
			return midIdx;
		}
	}

	return -(leftIdx + 1);
}