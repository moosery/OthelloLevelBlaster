#include "BP.h"

size_t BPGetDataCnt(PBPTree pTree)
{
	size_t valueToReturn;

	RWLockReadLock("BPGetDataCnt", &pTree->keyInfo.rwIdxLock);
	valueToReturn = pTree->keyInfo.stDataCnt;
	RWLockReadUnlock("BPGetDataCnt", &pTree->keyInfo.rwIdxLock);

	return valueToReturn;
}
