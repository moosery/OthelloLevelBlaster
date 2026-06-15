#include "BP.h"

void BPIterateStart(PBPTree pTree, PBPIterator pIterator)
{
	pIterator->isDone = false;
	pIterator->_pad[0] = pIterator->_pad[1] = pIterator->_pad[2] = pIterator->_pad[3] =
	pIterator->_pad[4] = pIterator->_pad[5] = pIterator->_pad[6] = 0;
	pIterator->currNode = NULL;
	pIterator->pTree = pTree;
	pIterator->nxtIdx = 0;

	if (pTree == NULL)
		pIterator->isDone = true;
	else
	{
		PBPIdxInfo pIdxInfo = &(pTree->keyInfo);
		BPRc rc = BP_RC_Success;

		/* First lock the tree */
		RWLockReadLock("BPIterateStart", &pTree->rwTreeLock);
		RWLockReadLock("BPIterateStart", &pIdxInfo->rwIdxLock);
		PBPNode pNode = pIdxInfo->pRootNode;
		RWLockReadUnlock("BPIterateStart", &pIdxInfo->rwIdxLock);

		RWLockReadLock("BPIterateStart", &pNode->rwNodeLock);
		RWLockReadUnlock("BPIterateStart", &pTree->rwTreeLock);

		while (BPIsKeyNode(pNode))
		{
			PBPNode pNewNode = pNode->ppChildArray[0];
			RWLockReadLock("BPIterateStart", &pNewNode->rwNodeLock);
			RWLockReadUnlock("BPIterateStart", &pNode->rwNodeLock);
			pNode = pNewNode;
		}

		pIterator->currNode = pNode;

		if (pNode->llNumInNode == 0)
		{
			RWLockReadUnlock("BPIterateStart", &pNode->rwNodeLock);
			pIterator->isDone = true;
		}
		else
		{
			pIterator->currNode = pNode;
		}
	}
}

void BPIterateStartFrom(PBPTree pTree, PBPIterator pIterator, void* pKeyData, bool returnEqual)
{
	pIterator->isDone  = false;
	pIterator->_pad[0] = pIterator->_pad[1] = pIterator->_pad[2] = pIterator->_pad[3] =
	pIterator->_pad[4] = pIterator->_pad[5] = pIterator->_pad[6] = 0;
	pIterator->currNode = NULL;
	pIterator->pTree   = pTree;
	pIterator->nxtIdx  = 0;

	if (pTree == NULL)
	{
		pIterator->isDone = true;
		return;
	}

	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	RWLockReadLock("BPIterateStartFrom", &pTree->rwTreeLock);
	RWLockReadLock("BPIterateStartFrom", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPIterateStartFrom", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPIterateStartFrom", &pNode->rwNodeLock);
	RWLockReadUnlock("BPIterateStartFrom", &pTree->rwTreeLock);

	/* Navigate to the correct leaf using the same top-down traversal as BPFindEqualKey */
	while (BPIsKeyNode(pNode))
	{
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx) - 1);
		else
			keyIdx++;

		PBPNode pNewNode = pNode->ppChildArray[keyIdx];
		RWLockReadLock("BPIterateStartFrom", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPIterateStartFrom", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	/* Locate the starting position within the leaf */
	BPLL startIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);

	if (startIdx >= 0)
	{
		/* Exact match — include it or skip past it based on returnEqual */
		if (!returnEqual)
			startIdx++;
	}
	else
	{
		/* Not found — insertion point is the first entry greater than the key */
		startIdx = (-startIdx) - 1;
	}

	/* If startIdx is past the end of this leaf, advance to the right sibling */
	if (startIdx >= pNode->llNumInNode)
	{
		PBPNode pRightNode = pNode->pRightSibling;
		if (pRightNode == NULL)
		{
			RWLockReadUnlock("BPIterateStartFrom", &pNode->rwNodeLock);
			pIterator->isDone = true;
			return;
		}
		RWLockReadLock("BPIterateStartFrom", &pRightNode->rwNodeLock);
		RWLockReadUnlock("BPIterateStartFrom", &pNode->rwNodeLock);
		pNode    = pRightNode;
		startIdx = 0;
	}

	/* Leave pNode's read lock held — BPIterate / BPIterateStop will release it */
	pIterator->currNode = pNode;
	pIterator->nxtIdx   = startIdx;
}

BPRc BPIterate(PBPIterator pIterator, void *pDataFound)
{
	BPRc rc = BP_RC_Success;
	void* pResult = NULL;
	PBPTree pTree = pIterator->pTree;

	if (pIterator->isDone)
		rc = BP_RC_Not_Found;
	else
	{
		PBPNode pCurrNode = pIterator->currNode;

		if (pIterator->nxtIdx >= pCurrNode->llNumInNode)
		{
			PBPNode pNxtNode = pCurrNode->pRightSibling;
			if (pNxtNode != NULL)
			{
				RWLockReadLock("BPIterate", &(pNxtNode->rwNodeLock));
				RWLockReadUnlock("BPIterate", &(pCurrNode->rwNodeLock));
				pCurrNode = pNxtNode;
				pIterator->currNode = pCurrNode;
				pIterator->nxtIdx = 0;
			}
			else
			{
				RWLockReadUnlock("BPIterate", &(pCurrNode->rwNodeLock));
				pIterator->isDone = true;
				pIterator->pTree = NULL;
				pIterator->currNode = NULL;
				pIterator->nxtIdx = 0;
				rc = BP_RC_Not_Found;
			}
		}

		if (!pIterator->isDone)
		{
			pResult = pCurrNode->ppDataPtrArray[pIterator->nxtIdx];
			(pIterator->nxtIdx)++;
		}
	}

	if (pResult != NULL)
		memcpy(pDataFound, pResult, pTree->stDataSize);
	else
		memset(pDataFound, 0, pTree->stDataSize);

	return rc;
}

void BPIterateStop(PBPIterator pIterator)
{
	if (!pIterator->isDone && pIterator->currNode != NULL)
	{
		RWLockReadUnlock("BPIterateStop", &(pIterator->currNode->rwNodeLock));
	}

	pIterator->isDone = true;
	pIterator->currNode = NULL;
	pIterator->nxtIdx = 0;
	pIterator->pTree = NULL;
}
