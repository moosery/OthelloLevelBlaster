#include "BP.h"
#include <stdlib.h>

BPRc BPSplitNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pOriginalNode, PBPNode* ppNewNode, void **ppDataToPushUp)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_INSERT))
	{
		fprintf(fpDebug, "Node being split (0x%p): \n", pOriginalNode);
		BPPrintNode(fpDebug, pTree, pOriginalNode);
	}
#endif

	/* Time to create a new sibling */
	BPRc rc = BPAllocateNode(pTree, pIdxInfo, ppNewNode, pOriginalNode->stNodeInfo & BP_NODETYPE_MASK);

	if (rc == BP_RC_Success)
	{
		/* No need to lock at this time since no one else knows about the new sibling! */

//		BPLL newNodeSize = (pTree->llOrder) / 2;
//		BPLL originalNodeNewSize = pOriginalNode->llNumInNode - newNodeSize;
		BPLL newNodeSize = pOriginalNode->llNumInNode - pTree->llMinKeys;
		BPLL originalNodeNewSize = pTree->llMinKeys;
		PBPNode pNewNode = *ppNewNode;
		BPLL indexToCopyFrom;

		*ppDataToPushUp = pOriginalNode->ppDataPtrArray[originalNodeNewSize];
		if (BPIsKeyNode(pNewNode))
		{
			(newNodeSize--);
			indexToCopyFrom = originalNodeNewSize + 1;
		}
		else
		{
			indexToCopyFrom = originalNodeNewSize;
		}

		pNewNode->llNumInNode = newNodeSize;
		pNewNode->pParent = pOriginalNode->pParent;
		pNewNode->pLeftSibling = pOriginalNode;
		pNewNode->pRightSibling = pOriginalNode->pRightSibling;
		pOriginalNode->pRightSibling = pNewNode;

		memcpy(&(pNewNode->ppDataPtrArray[0]), &(pOriginalNode->ppDataPtrArray[indexToCopyFrom]), newNodeSize * sizeof(char*));

		if (BPIsKeyNode(pNewNode))
		{
			for (BPLL childIdx = 0; childIdx < newNodeSize + 1; childIdx++)
			{
				pNewNode->ppChildArray[childIdx] = pOriginalNode->ppChildArray[indexToCopyFrom + childIdx];
				RWLockWriteLock("BPSplitNode-childIdx", & (pNewNode->ppChildArray[childIdx]->rwNodeLock));
				pNewNode->ppChildArray[childIdx]->pParent = pNewNode;
				RWLockWriteUnlock("BPSplitNode-childIdx", &(pNewNode->ppChildArray[childIdx]->rwNodeLock));
			}
		}

		/* Now adjust the size of the original node */
		pOriginalNode->llNumInNode = originalNodeNewSize;

		/* Now attach the right sibling to the new node */
		if (pNewNode->pRightSibling != NULL)
		{
			RWLockWriteLock("BPSplitNode-RightSib", &(pNewNode->pRightSibling->rwNodeLock));
			pNewNode->pRightSibling->pLeftSibling = pNewNode;
			RWLockWriteUnlock("BPSplitNode-RightSib", &(pNewNode->pRightSibling->rwNodeLock));
		}

#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_INSERT))
		{
			fprintf(fpDebug, "After being split (0x%p) with right node (0x%p): \n", pOriginalNode,pNewNode);
			BPPrintNode(fpDebug, pTree, pOriginalNode);
			fprintf(fpDebug, "The new node is: \n");
			BPPrintNode(fpDebug, pTree, pNewNode);
			fprintf(fpDebug, "The data being pushed to parent is: '");
			BPPrintKeyAtAddress(fpDebug, pTree, *ppDataToPushUp);
			fprintf(fpDebug, "'\n");
		}
#endif

	}

	return rc;
}

BPRc BPInsertIntoNotFullNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode, void *pData)
{
	BPRc rc = BP_RC_Success;

	if (BPIsLeafNode(pNode))
	{
		BPLL idxToInsert = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pData);

		if (idxToInsert >= 0)
		{
			RWLockWriteUnlock("BPInsertIntoNotFullNode-node on Dup", &(pNode->rwNodeLock));
			return(BP_RC_Duplicate_Found);
		}

		/* Get the insert location */
		idxToInsert = (-idxToInsert) - 1;
		BPLL numToMoveDown = pNode->llNumInNode - idxToInsert;

		if (pTree->pArena)
		{
			RWLockWriteLock("BPInsertIntoNotFullNode-idx", &pIdxInfo->rwIdxLock);
			(pIdxInfo->stDataCnt)++;
			RWLockWriteUnlock("BPInsertIntoNotFullNode-idx", &pIdxInfo->rwIdxLock);

			/* Now copy things down.  Remember, we know we can do this since it has space! */
			memmove(&(pNode->ppDataPtrArray[idxToInsert + 1]), &(pNode->ppDataPtrArray[idxToInsert]), numToMoveDown * sizeof(char*));
			pNode->ppDataPtrArray[idxToInsert] = (char*)pData;
			(pNode->llNumInNode)++;
		}
		else
		{
			bool isFull = false;
			RWLockWriteLock("BPInsertIntoNotFullNode-idx", &pIdxInfo->rwIdxLock);
			if (pIdxInfo->stDataCnt > pIdxInfo->stMaxDataCnt)
			{
				fprintf(stderr, "******************************************************************\n");
				fprintf(stderr, "****** Somehow a tree has more info than it should (%zu vs %zu)!\n", pIdxInfo->stDataCnt, pIdxInfo->stMaxDataCnt);
				fprintf(stderr, "******************************************************************\n");
				fflush(stderr);
			}
			if (pIdxInfo->stDataCnt >= pIdxInfo->stMaxDataCnt)
				isFull = true;
			else
				(pIdxInfo->stDataCnt)++;
			RWLockWriteUnlock("BPInsertIntoNotFullNode-idx", &pIdxInfo->rwIdxLock);

			if (!isFull)
			{
				/* Now copy things down.  Remember, we know we can do this since it has space! */
				memmove(&(pNode->ppDataPtrArray[idxToInsert + 1]), &(pNode->ppDataPtrArray[idxToInsert]), numToMoveDown * sizeof(char*));
				pNode->ppDataPtrArray[idxToInsert] = (char*)pData;
				(pNode->llNumInNode)++;
			}
			else
				rc = BP_RC_Tree_Full;
		}

		RWLockWriteUnlock("BPInsertIntoNotFullNode-node", & (pNode->rwNodeLock));
	}
	else
	{
		/* Keep hunting for the leaf to insert, splitting as needed along the way. */
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pData);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx)-1);
		else
			keyIdx++;

		/* We now need to lock the child */
		PBPNode pChildNode = pNode->ppChildArray[keyIdx];
		RWLockWriteLock("BPInsertIntoNotFullNode-child", &pChildNode->rwNodeLock);

		/* Split the darn thing if we need to */
		if (pChildNode->llNumInNode == (pTree->llOrder-1))
		{
			PBPNode pNewNode;
			void* pKeyToStore;

			rc = BPSplitNode(pTree, pIdxInfo, pChildNode, &pNewNode, &pKeyToStore);

			if (rc == BP_RC_Success)
			{
				BPLL idxToInsert = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pKeyToStore);

				if (idxToInsert >= 0)
				{
					Fatal(FATAL_BP_DUP_KEY,"**************** SERIOUS BAD ERROR!  Dups found in key????\n");
				}

				idxToInsert = (-idxToInsert) - 1;
				BPLL numtoCopyDown = pNode->llNumInNode - idxToInsert;
				memmove(&(pNode->ppDataPtrArray[idxToInsert + 1]), &(pNode->ppDataPtrArray[idxToInsert]), numtoCopyDown * sizeof(char*));
				pNode->ppDataPtrArray[idxToInsert] = (char *) pKeyToStore;
				memmove(&(pNode->ppChildArray[idxToInsert + 2]), &(pNode->ppChildArray[idxToInsert+1]), numtoCopyDown * sizeof(PBPNode));
				pNode->ppChildArray[idxToInsert + 1] = pNewNode;
				(pNode->llNumInNode)++;

				/* Now see which child we need to insert into */
				if (BPKeyCmpPP(pTree, pIdxInfo, pData, pKeyToStore) >= 0)
				{
					RWLockWriteLock("BPInsertIntoNotFullNode-newnode", &pNewNode->rwNodeLock);
					RWLockWriteUnlock("BPInsertIntoNotFullNode-child", &pChildNode->rwNodeLock);
					pChildNode = pNewNode;
				}
			}

		}

		RWLockWriteUnlock("BPInsertIntoNotFullNode-node", &pNode->rwNodeLock);

		if (rc == BP_RC_Success)
		{
			rc = BPInsertIntoNotFullNode(pTree, pIdxInfo, pChildNode, pData);
		}
		else
		{
			RWLockWriteUnlock("BPInsertIntoNotFullNode-child on failure",&pChildNode->rwNodeLock);
		}
	}

	return rc;
}

/*
** BPInsertDataPtr — OWNERSHIP TRANSFER.
** The pointer pData is stored directly in the tree.  The tree takes ownership and will
** call MemFree(pData) when the entry is deleted via BPDeleteDataAndFree.
** Pair exclusively with BPDeleteDataAndFree (frees the data) — never with
** BPDeleteDataNoFree, which would leak.  If you do not want to transfer ownership,
** use BPInsertCopy instead; it allocates its own copy and always pairs correctly with
** BPDeleteDataAndFree.
*/
BPRc BPInsertDataPtr(PBPTree pTree, void* pData)
{
	BPRc rc = BP_RC_Success;

	if (pTree->pArena && ArenaMemHasOverflowed(pTree->pArena))
	{
		rc = BP_RC_Tree_Full;
	}
	else
	{

		/* Grab the lock on the tree */
		RWLockWriteLock("BPInsertDataPtr-tree", &pTree->rwTreeLock);
		PBPIdxInfo pIdxInfo = &pTree->keyInfo;


		RWLockReadLock("BPInsertDataPtr-idx", &pIdxInfo->rwIdxLock);
		PBPNode pRoot = pTree->keyInfo.pRootNode;
		RWLockReadUnlock("BPInsertDataPtr-idx", &pIdxInfo->rwIdxLock);

		/* Now lock the root ptr in the index */
		RWLockWriteLock("BPInsertDataPtr-root", &pRoot->rwNodeLock);

		/* Now check the root node to see if it needs to be split */

		if (pRoot->llNumInNode == (pTree->llOrder - 1))
		{
			PBPNode pNewRoot;

			/* Time to create a new root */
			rc = BPAllocateNode(pTree, pIdxInfo, &pNewRoot, BP_NODEINFO_KEY);

			if (rc == BP_RC_Success)
			{
				/* Split the old root which will create yet another node. */
				PBPNode pNewSplitNode;
				void* pDataToInsertIntoRoot;

				rc = BPSplitNode(pTree, pIdxInfo, pRoot, &pNewSplitNode, &pDataToInsertIntoRoot);

				if (rc == BP_RC_Success)
				{
					pNewRoot->llNumInNode = 1;
					pNewRoot->pParent = NULL;
					pNewRoot->pLeftSibling = NULL;
					pNewRoot->pRightSibling = NULL;
					pNewRoot->ppDataPtrArray[0] = (char*)pDataToInsertIntoRoot;
					pNewRoot->ppChildArray[0] = pRoot;
					pNewRoot->ppChildArray[1] = pNewSplitNode;
					pRoot->pParent = pNewRoot;
					pNewSplitNode->pParent = pNewRoot;
					RWLockWriteLock("BPInsertDataPtr-idx", &pIdxInfo->rwIdxLock);
					pIdxInfo->pRootNode = pNewRoot;
					(pIdxInfo->stIdxDepth)++;
					RWLockWriteUnlock("BPInsertDataPtr-idx", &pIdxInfo->rwIdxLock);
					RWLockWriteUnlock("BPInsertDataPtr-oldroot", &pRoot->rwNodeLock);
					RWLockWriteLock("BPInsertDataPtr-newroot", &pNewRoot->rwNodeLock);
					pRoot = pNewRoot;
				}
				else
				{
					BPFreeNode(pTree, pIdxInfo, pNewRoot);
				}
			}
		}
		/* Unlock the lock on the tree */
		RWLockWriteUnlock("BPInsertDataPtr-tree", &pTree->rwTreeLock);

		if (rc == BP_RC_Success)
		{
			/* Note, the insert into the not full node will unlock the node lock */
			rc = BPInsertIntoNotFullNode(pTree, pIdxInfo, pRoot, pData);
		}
		else
		{
			RWLockWriteUnlock("BPInsertDataPtr-root on error", &pRoot->rwNodeLock);
		}
	}

	return rc;

}

BPRc BPInsertCopy(PBPTree pTree, void* pData)
{
	char* pStorableData = pTree->pArena
		? (char*)ArenaMemMalloc(pTree->pArena, "DataEntry", SizeofDataEntry(pTree))
		: (char*)MemMalloc("DataEntry", SizeofDataEntry(pTree));

	if (pStorableData == NULL)
		return BP_RC_Allocate_Failed;

	memcpy(pStorableData, pData, pTree->stDataSize);

	BPRc rc = BPInsertDataPtr(pTree, pStorableData);

	if (rc != BP_RC_Success)
	{
		if (pTree->pArena) ArenaMemFree(pTree->pArena, pStorableData);
		else               MemFree(pStorableData);
	}

	return(rc);
}
