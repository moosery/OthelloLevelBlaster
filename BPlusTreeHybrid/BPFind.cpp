#include "BP.h"
#include <thread>

BPLL BPFindNodeDataBinary(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode, void* pData)
{
	BPLL leftIdx = 0;
	BPLL rightIdx = pNode->llNumInNode - 1;
	BPLL midIdx = 0;
	char** ppDataPtrArray = pNode->ppDataPtrArray;


	while (leftIdx <= rightIdx)
	{
		midIdx = leftIdx + ((rightIdx - leftIdx) >> 1);

		int cmpVal = BPKeyCmpPP(pTree, pIdxInfo, ppDataPtrArray[midIdx], pData);
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

BPRc BPFindEqualKey(PBPTree pTree, void* pKeyData, void *pDataFound)
{
	BPRc rc = BP_RC_Success;
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	/* First lock the tree */
	RWLockReadLock("BPFindEqualKey", &pTree->rwTreeLock);
	RWLockReadLock("BPFindEqualKey", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPFindEqualKey", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPFindEqualKey", &pNode->rwNodeLock);

	RWLockReadUnlock("BPFindEqualKey", &pTree->rwTreeLock);

	/* Find the node where the key is or where the key should go */
	while (BPIsKeyNode(pNode))
	{
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx)-1);
		else
			keyIdx++;

		/* Yes - remember the ppChildArray contains 1 more than the max! */
		PBPNode pNewNode = pNode->ppChildArray[keyIdx];
		RWLockReadLock("BPFindEqualKey", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPFindEqualKey", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	BPLL llFoundDataOffsetIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);

	if (llFoundDataOffsetIdx >= 0)
	{
		memcpy(pDataFound, pNode->ppDataPtrArray[llFoundDataOffsetIdx], pTree->stDataSize);
	}
	else
	{
		rc = BP_RC_Not_Found;
		memset(pDataFound, 0, pTree->stDataSize);
	}
	RWLockReadUnlock("BPFindEqualKey", &pNode->rwNodeLock);


	return rc;
}
BPRc BPFindFirstKey(PBPTree pTree, void* pDataFound)
{
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);
	BPRc rc = BP_RC_Success;
	memset(pDataFound, 0, pTree->stDataSize);

	/* First lock the tree */
	RWLockReadLock("BPFindFirstKey", &pTree->rwTreeLock);
	RWLockReadLock("BPFindFirstKey", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPFindFirstKey", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPFindFirstKey", &pNode->rwNodeLock);
	RWLockReadUnlock("BPFindFirstKey", &pTree->rwTreeLock);

	while (BPIsKeyNode(pNode))
	{
		PBPNode pNewNode = pNode->ppChildArray[0];
		RWLockReadLock("BPFindFirstKey", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPFindFirstKey", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	if (pNode->llNumInNode > 0)
	{
		memcpy(pDataFound, pNode->ppDataPtrArray[0], pTree->stDataSize);
	}
	else
	{
		rc = BP_RC_Not_Found;
	}

	RWLockReadUnlock("BPFindFirstKey", &pNode->rwNodeLock);
	return rc;
}

BPRc BPFindLastKey(PBPTree pTree, void* pDataFound)
{
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);
	BPRc rc = BP_RC_Success;
	memset(pDataFound, 0, pTree->stDataSize);

	/* First lock the tree */
	RWLockReadLock("BPFindLastKey", &pTree->rwTreeLock);
	RWLockReadLock("BPFindLastKey", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPFindLastKey", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPFindLastKey", &pNode->rwNodeLock);
	RWLockReadUnlock("BPFindLastKey", &pTree->rwTreeLock);

	while (BPIsKeyNode(pNode))
	{
		PBPNode pNewNode = pNode->ppChildArray[pNode->llNumInNode];
		RWLockReadLock("BPFindLastKey", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPFindLastKey", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	if (pNode->llNumInNode > 0)
	{
		memcpy(pDataFound, pNode->ppDataPtrArray[((pNode->llNumInNode) - 1)], pTree->stDataSize);
	}
	else
	{
		rc = BP_RC_Not_Found;
	}

	RWLockReadUnlock("BPFindLastKey", &pNode->rwNodeLock);
	return rc;
}

BPRc BPFindGreaterThanKey(PBPTree pTree, void* pKeyData, void* pDataFound, bool returnEqual)
{
	BPRc rc = BP_RC_Success;
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	/* First lock the tree */
	RWLockReadLock("BPFindGreaterThanKey", &pTree->rwTreeLock);
	RWLockReadLock("BPFindGreaterThanKey", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPFindGreaterThanKey", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPFindGreaterThanKey", &pNode->rwNodeLock);

	RWLockReadUnlock("BPFindGreaterThanKey", &pTree->rwTreeLock);

	/* Find the node where the key is or where the key should go */
	while (BPIsKeyNode(pNode))
	{
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx)-1);
		else
			keyIdx++;

		/* Yes - remember the ppChildArray contains 1 more than the max! */
		PBPNode pNewNode = pNode->ppChildArray[keyIdx];
		RWLockReadLock("BPFindGreaterThanKey", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPFindGreaterThanKey", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	/* Now that we found the node, we need to hunt for the key passed in */
	/* For now, we do a linear search for the value.  We can improve by doing a binary search here since the data will be sorted. */
	BPLL llFoundDataOffsetIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);

	/* Now get the data and rec id */
	if (llFoundDataOffsetIdx >= 0)
	{
		if (!returnEqual)
		{
			/* Remember, if we found it, then we want Greater.  So we want the data following the one being pointed to. */
			/* So if we add one to the found offset, and it's greater than the number in the node, we need to look to the right */
			/* If the right doesn't exist, whelp, it aint there!!!!*/
			if (llFoundDataOffsetIdx + 1 >= pNode->llNumInNode)
			{
				PBPNode pRightNode = pNode->pRightSibling;
				if (pRightNode == NULL)
					rc = BP_RC_Not_Found;
				else
				{
					RWLockReadLock("BPFindGreaterThanKey", &(pRightNode->rwNodeLock));
					RWLockReadUnlock("BPFindGreaterThanKey", &pNode->rwNodeLock);
					llFoundDataOffsetIdx = 0;
					pNode = pRightNode;
				}
			}
			else
				llFoundDataOffsetIdx += 1;
		}

		if (rc == BP_RC_Success)
		{
			memcpy(pDataFound, pNode->ppDataPtrArray[llFoundDataOffsetIdx], pTree->stDataSize);
		}

		RWLockReadUnlock("BPFindGreaterThanKey", &(pNode->rwNodeLock));
	}
	else
	{
		if (pNode->llNumInNode != 0)
		{
			llFoundDataOffsetIdx = (-llFoundDataOffsetIdx) - 1;

			/* NOTE: The llFoundDataOffsetIdx is actually the idx of where it would be inserted.  Meaning the value at the index is actually greater than the key passed in (if the idx is not larger than the llNumInNode) */
			if (llFoundDataOffsetIdx >= pNode->llNumInNode)
			{
				/* Again, we want greater, and evidently, that is in the next node.  So, we gotta go right again. */
				PBPNode pRightNode = pNode->pRightSibling;
				if (pRightNode == NULL)
					rc = BP_RC_Not_Found;
				else
				{
					RWLockReadLock("BPFindGreaterThanKey", &(pRightNode->rwNodeLock));
					RWLockReadUnlock("BPFindGreaterThanKey", &pNode->rwNodeLock);
					llFoundDataOffsetIdx = 0;
					pNode = pRightNode;
				}

				if (rc == BP_RC_Success)
				{
					memcpy(pDataFound, pNode->ppDataPtrArray[llFoundDataOffsetIdx], pTree->stDataSize);
				}


				RWLockReadUnlock("BPFindGreaterThanKey", &(pNode->rwNodeLock));
			}
			else
			{
				/* We just need to return the data at the found offset */
				memcpy(pDataFound, pNode->ppDataPtrArray[llFoundDataOffsetIdx], pTree->stDataSize);
				RWLockReadUnlock("BPFindGreaterThanKey", &(pNode->rwNodeLock));
			}
		}
		else
		{
			rc = BP_RC_Not_Found;
			RWLockReadUnlock("BPFindGreaterThanKey", &pNode->rwNodeLock);
		}
	}

	if (rc != BP_RC_Success)
	{
		memset(pDataFound, 0, pTree->stDataSize);
	}

	return rc;
}

/* Should try to not use this since going less can cause a deadlock prevention error! */
static BPRc BPFindLessThanKeyOnce(PBPTree pTree, void* pKeyData, void* pDataFound, bool returnEqual)
{
	BPRc rc = BP_RC_Success;
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	/* First lock the tree */
	RWLockReadLock("BPFindLessThanKey", &pTree->rwTreeLock);
	RWLockReadLock("BPFindLessThanKey", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPFindLessThanKey", &pIdxInfo->rwIdxLock);

	RWLockReadLock("BPFindLessThanKey", &pNode->rwNodeLock);

	RWLockReadUnlock("BPFindLessThanKey", &pTree->rwTreeLock);

	/* Find the node where the key is or where the key should go */
	while (BPIsKeyNode(pNode))
	{
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx)-1);
		else
			keyIdx++;

		/* Yes - remember the ppChildArray contains 1 more than the max! */
		PBPNode pNewNode = pNode->ppChildArray[keyIdx];
		RWLockReadLock("BPFindLessThanKey", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPFindLessThanKey", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	/* Now that we found the node, we need to hunt for the key passed in */
	/* For now, we do a linear search for the value.  We can improve by doing a binary search here since the data will be sorted. */
	BPLL llFoundDataOffsetIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyData);
	bool returnLastInPrevNode = false;

	if (pNode->llNumInNode == 0)
		rc = BP_RC_Not_Found;
	else if (llFoundDataOffsetIdx >= 0)
	{
		if (returnEqual)
		{
			if (llFoundDataOffsetIdx >= pNode->llNumInNode)
				llFoundDataOffsetIdx--;
		}
		else if (llFoundDataOffsetIdx > 0)
			llFoundDataOffsetIdx--;
		else
			returnLastInPrevNode = true;
	}
	else
	{
		llFoundDataOffsetIdx = (-llFoundDataOffsetIdx) - 1;

		/* The value is actually the idx of where it would be inserted which means the value AT the index is greater than the key (if the idx is not larger than the NumInNode) */
		/* So as long as its greater then zero, all we have to do is subtract one */
		if (llFoundDataOffsetIdx > 0)
			llFoundDataOffsetIdx--;
		else
		{
			/* We gotta go left to find the less than!!! */
			returnLastInPrevNode = true;
		}
	}


	if (rc == BP_RC_Success)
	{
		/* See if we need to go to the prev (left) node */
		if (returnLastInPrevNode)
		{
			/* This is scary, because it can cause a deadlock.  So instead of just grabbing the lock, we try to grab it.  If it fails, too bad, we fail the entire query. */
			if (pNode->pLeftSibling == NULL)
				rc = BP_RC_Not_Found;
			else
			{
				PBPNode pLeftNode = pNode->pLeftSibling;

				if (RWLockReadTryLock("BPFindLessThanKey", &(pLeftNode->rwNodeLock), 5))
				{
					RWLockReadUnlock("BPFindLessThanKey", &pNode->rwNodeLock);
					pNode = pLeftNode;
					llFoundDataOffsetIdx = pNode->llNumInNode - 1;
				}
				else
					rc = BP_RC_Deadlock_Prevention;
			}
		}

		if (rc == BP_RC_Success)
		{
			memcpy(pDataFound, pNode->ppDataPtrArray[llFoundDataOffsetIdx], pTree->stDataSize);
		}
	}

	RWLockReadUnlock("BPFindLessThanKey", &pNode->rwNodeLock);

	if (rc != BP_RC_Success && rc != BP_RC_Deadlock_Prevention)
	{
		memset(pDataFound, 0, pTree->stDataSize);
	}

	return rc;
}

BPRc BPFindLessThanKey(PBPTree pTree, void* pKeyData, void* pDataFound, bool returnEqual)
{
	/* When reaching the leftmost entry of a leaf the traversal must acquire the left
	** sibling's lock in the non-preferred direction.  If the trylock times out, all
	** locks are already released, so it is safe to yield and retry from the root. */
	const int MAX_RETRIES = 5;
	BPRc rc = BP_RC_Deadlock_Prevention;
	for (int attempt = 0; attempt < MAX_RETRIES && rc == BP_RC_Deadlock_Prevention; attempt++)
	{
		if (attempt > 0)
			std::this_thread::yield();
		rc = BPFindLessThanKeyOnce(pTree, pKeyData, pDataFound, returnEqual);
	}
	return rc;
}
