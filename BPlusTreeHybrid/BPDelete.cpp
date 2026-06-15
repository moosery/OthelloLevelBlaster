#include "BP.h"
#include <thread>
BPRc BPRemoveFromNonUnderflowLeaf(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pLeafNode, void* pData, BPLL idxOfInterest, PBPNode pFoundKeyNode, BPLL foundKeyNodeIdxOfInterest, bool freeData)
{
#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPRemoveFromNonUnderflowLeaf: 0x%p is a leaf node. So deleting entry at idx %zd\n", pLeafNode, idxOfInterest);
		fprintf(fpDebug, "BPRemoveFromNonUnderflowLeaf: The node looks like this:\n");
		BPPrintNode(fpDebug, pTree, pLeafNode);
		if (pLeafNode->pParent != NULL)
		{
			fprintf(fpDebug, "BPRemoveFromNonUnderflowLeaf: The parent node looks like this:\n");
			BPPrintNode(fpDebug, pTree, pLeafNode->pParent);
		}

		fflush(fpDebug);
	}
#endif

	if (idxOfInterest < 0)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPRemoveFromNonUnderflowLeaf: returning not found!\n");
			fflush(fpDebug);
		}
#endif
		RWLockWriteUnlock("BPRemoveFromNonUnderflowLeaf-leaf", &pLeafNode->rwNodeLock);

		return BP_RC_Not_Found;
	}

	char* pDataItem = pLeafNode->ppDataPtrArray[idxOfInterest];

	/* Otherwise simply remove the data.  */
	BPLL numToCopyDown = (pLeafNode->llNumInNode - idxOfInterest) - 1;

	memmove(&(pLeafNode->ppDataPtrArray[idxOfInterest]), &(pLeafNode->ppDataPtrArray[idxOfInterest + 1]), numToCopyDown * sizeof(char*));
	(pLeafNode->llNumInNode)--;

	if (idxOfInterest == 0)
	{
		if (pFoundKeyNode != NULL)
		{
			if (foundKeyNodeIdxOfInterest >= 0)
			{
				pFoundKeyNode->ppDataPtrArray[foundKeyNodeIdxOfInterest] = pLeafNode->ppDataPtrArray[0];
				RWLockWriteUnlock("BPRemoveFromNonUnderflowLeaf-key", &pFoundKeyNode->rwNodeLock);
			}
#ifdef DEBUG_ON
			else
			{
				Fatal(FATAL_BP_DELETE, "******** Oh well - you tried to do the delete, and you failed.   This sucks.  The found node was 0x%p and the offset was %zd\n", pFoundKeyNode, foundKeyNodeIdxOfInterest);
			}
#endif
		}
#ifdef DEBUG_ON
		else
		{
			if (pLeafNode->pParent != NULL && pLeafNode->pLeftSibling != NULL)
			{
				Fatal(FATAL_BP_DELETE, "******** Oh well - you tried to do the delete, and you failed.   This sucks 2.  The found node was 0x%p and the offset was %zd\n", pFoundKeyNode, foundKeyNodeIdxOfInterest);
			}
		}
#endif

	}

	RWLockWriteUnlock("BPRemoveFromNonUnderflowLeaf-leaf", &pLeafNode->rwNodeLock);

	if (freeData)
	{
		if (pTree->pArena) ArenaMemFree(pTree->pArena, pDataItem);
		else               MemFree(pDataItem);
	}

	RWLockWriteLock("BPRemoveFromNonUnderflowLeaf-idx", &pIdxInfo->rwIdxLock);
	if (pIdxInfo->stDataCnt == 0)
		Fatal(FATAL_BP_DELETE, "BPRemoveFromNonUnderflowLeaf: stDataCnt underflow — attempted to decrement below zero\n");
	else
		(pIdxInfo->stDataCnt)--;
	RWLockWriteUnlock("BPRemoveFromNonUnderflowLeaf-idx", &pIdxInfo->rwIdxLock);

	return BP_RC_Success;
}

BPRc BPMergeOrStealChildKeyNodes(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pParent, BPLL childTraversalIdx, PBPNode pLeftChild, PBPNode *ppChild, PBPNode pRightChild, bool freeData)
{
	PBPNode pChild = *ppChild;
	BPRc rc = BP_RC_Success;

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: Entered\n");
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The childTraversalIdx is %zd\n", childTraversalIdx);
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The parent (0x%016p) is as follows:\n",pParent);
		BPPrintNode(fpDebug, pTree, pParent);
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The left child (0x%016p) is as follows:\n",pLeftChild);
		BPPrintNode(fpDebug, pTree, pLeftChild);
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The child (0x%016p) is as follows:\n", pChild);
		BPPrintNode(fpDebug, pTree, pChild);
		fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The right child (0x%016p) is as follows:\n", pRightChild);
		BPPrintNode(fpDebug, pTree, pRightChild);
		fflush(fpDebug);
	}
#endif

	/* Prefer to steal from right first */
	if (pRightChild != NULL && pRightChild->llNumInNode > pTree->llMinKeys)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: We are stealing from the right node!\n");
			fflush(fpDebug);

		}
#endif
		/* First, add the parent's key value to the child. */
		pChild->ppDataPtrArray[pChild->llNumInNode] = pParent->ppDataPtrArray[childTraversalIdx];
		/* Now add the left most ptr value from right to child */
		pChild->ppChildArray[pChild->llNumInNode + 1] = pRightChild->ppChildArray[0];
		/* Increase the child's count */
		(pChild->llNumInNode)++;
		/* Now copy the first value of the right into the parent */
		pParent->ppDataPtrArray[childTraversalIdx] = pRightChild->ppDataPtrArray[0];

		/* Remove one from the right child */
		(pRightChild->llNumInNode)--;

		/* Now shift EVERYTHING down in the right key node */
		memmove(&(pRightChild->ppDataPtrArray[0]), &(pRightChild->ppDataPtrArray[1]), pRightChild->llNumInNode * sizeof(char*));
		memmove(&(pRightChild->ppChildArray[0]), &(pRightChild->ppChildArray[1]), (pRightChild->llNumInNode+1) * sizeof(PBPNode));

		/* Gotta fix the parentage of the child moved */
		RWLockWriteLock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pChild->ppChildArray[pChild->llNumInNode]->rwNodeLock));
		pChild->ppChildArray[pChild->llNumInNode]->pParent = pChild;
		RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pChild->ppChildArray[pChild->llNumInNode]->rwNodeLock));

		if(pLeftChild != NULL)
			RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-leftChild", &pLeftChild->rwNodeLock);
		RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-RightChild", &pRightChild->rwNodeLock);

#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: After the right steal!\n");
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The parent (0x%016p) is as follows:\n", pParent);
			BPPrintNode(fpDebug, pTree, pParent);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
			BPPrintNode(fpDebug, pTree, pLeftChild);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The child (0x%016p) is as follows:\n", pChild);
			BPPrintNode(fpDebug, pTree, pChild);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The right child (0x%016p) is as follows:\n", pRightChild);
			BPPrintNode(fpDebug, pTree, pRightChild);
			fflush(fpDebug);
		}
#endif
	}
	else if(pLeftChild != NULL && pLeftChild->llNumInNode > pTree->llMinKeys)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: We are stealing from the left node!\n");
			fflush(fpDebug);
		}
#endif
		/* We gotta shift everything to the right in the child. */
		memmove(&(pChild->ppDataPtrArray[1]), &(pChild->ppDataPtrArray[0]), pChild->llNumInNode * sizeof(char*));
		memmove(&(pChild->ppChildArray[1]), &(pChild->ppChildArray[0]), (pChild->llNumInNode+1) * sizeof(PBPNode));

		/* Add the parent's key value to the child */
		pChild->ppDataPtrArray[0] = pParent->ppDataPtrArray[childTraversalIdx - 1];
		/* add the right most ptr value from left to the child */
		pChild->ppChildArray[0] = pLeftChild->ppChildArray[pLeftChild->llNumInNode];
		/* Increase the child's count */
		(pChild->llNumInNode)++;

		/* Remove one from the left  child */
		(pLeftChild->llNumInNode)--;

		/* Now copy the last value of the left into the parent */
		pParent->ppDataPtrArray[childTraversalIdx - 1] = pLeftChild->ppDataPtrArray[pLeftChild->llNumInNode];

		/* Gotta fix the parentage of the child moved */
		RWLockWriteLock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pChild->ppChildArray[0]->rwNodeLock));
		pChild->ppChildArray[0]->pParent = pChild;
		RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pChild->ppChildArray[0]->rwNodeLock));

		RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-leftChild", &pLeftChild->rwNodeLock);
		if (pRightChild != NULL)
			RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-RightChild", &pRightChild->rwNodeLock);
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: After the left steal!\n");
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The parent (0x%016p) is as follows:\n", pParent);
			BPPrintNode(fpDebug, pTree, pParent);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
			BPPrintNode(fpDebug, pTree, pLeftChild);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The child (0x%016p) is as follows:\n", pChild);
			BPPrintNode(fpDebug, pTree, pChild);
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The right child (0x%016p) is as follows:\n", pRightChild);
			BPPrintNode(fpDebug, pTree, pRightChild);
			fflush(fpDebug);
		}
#endif
	}
	else if (pLeftChild != NULL)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: We are merging the child with the left child!\n");
			fflush(fpDebug);
		}
#endif
		/* We really need to lock the right child first so that we can update sibling linkage.  RightChild passed in might be null if the right sibling */
		/* is under a different parent.  We need to check this and try to obtain the lock.  If we cannot obtain the lock, give up and claim failure due to deadlock prevention. */
		if (pRightChild == NULL && pChild->pRightSibling != NULL)
		{
			if (RWLockWriteTryLock("BPMergeOrStealChildKeyNodes-RightChildUnderDiffParent", &(pChild->pRightSibling->rwNodeLock), 5))
			{
				pRightChild = pChild->pRightSibling;
			}
			else
			{
				/* We need to fail, which means we need to release the left lock and return an error */
				rc = BP_RC_Deadlock_Prevention;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes->leftchild", &pLeftChild->rwNodeLock);
			}
		}

		if (rc == BP_RC_Success)
		{

			/* We have to copy the parent's data value into the last value of the left. */
			pLeftChild->ppDataPtrArray[pLeftChild->llNumInNode] = pParent->ppDataPtrArray[childTraversalIdx - 1];
			BPLL idxOfNewChildPtrs = pLeftChild->llNumInNode + 1;

			/* Merge child stuff into the left node.  */
			memcpy(&(pLeftChild->ppDataPtrArray[pLeftChild->llNumInNode + 1]), &(pChild->ppDataPtrArray[0]), pChild->llNumInNode * sizeof(char*));
			memcpy(&(pLeftChild->ppChildArray[pLeftChild->llNumInNode + 1]), &(pChild->ppChildArray[0]), (pChild->llNumInNode + 1) * sizeof(PBPNode));
			(pLeftChild->llNumInNode) += (1 + pChild->llNumInNode);

			/* Now nuke the entry from the parent */
			memmove(&(pParent->ppDataPtrArray[childTraversalIdx - 1]), &(pParent->ppDataPtrArray[childTraversalIdx]), ((pParent->llNumInNode - childTraversalIdx)) * sizeof(char*));
			memmove(&(pParent->ppChildArray[childTraversalIdx]), &(pParent->ppChildArray[childTraversalIdx + 1]), ((pParent->llNumInNode - childTraversalIdx)) * sizeof(PBPNode));
			(pParent->llNumInNode)--;

			/* Fix left/right sibling ptrs */
			pLeftChild->pRightSibling = pRightChild;
			if (pRightChild != NULL)
				pRightChild->pLeftSibling = pLeftChild;

			/* Now we gotta fix parentage of all the child ptrs copied */
			for (BPLL idx = idxOfNewChildPtrs; idx < (pLeftChild->llNumInNode + 1); idx++)
			{
				RWLockWriteLock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pLeftChild->ppChildArray[idx]->rwNodeLock));
				pLeftChild->ppChildArray[idx]->pParent = pLeftChild;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pLeftChild->ppChildArray[idx]->rwNodeLock));
			}

			/* Now we gotta nuke the child node */
			pChild->llNumInNode = 0;
			RWLockWriteUnlock("BPMergeOrStealChildKeyNodes - child for removal", &pChild->rwNodeLock);
			BPFreeNode(pTree, pIdxInfo, pChild);

			if (pRightChild != NULL)
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-RightChild", &pRightChild->rwNodeLock);

#ifdef DEBUG_ON
			if (IsDebugging(DEBUG_DELETE))
			{
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: After the left merge!\n");
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The parent (0x%016p) is as follows:\n", pParent);
				BPPrintNode(fpDebug, pTree, pParent);
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
				BPPrintNode(fpDebug, pTree, pLeftChild);
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The right child (0x%016p) is as follows:\n", pRightChild);
				BPPrintNode(fpDebug, pTree, pRightChild);
				fflush(fpDebug);
			}
#endif


			* ppChild = pLeftChild;
		}
	}
	else
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: We are merging the child with the right child!\n");
			fflush(fpDebug);
		}
#endif

		/* We really need to lock the left child first so that we can update sibling linkage.  LeftChild passed in might be null if the left sibling */
	    /* is under a different parent.  We need to check this and try to obtain the lock.  If we cannot obtain the lock, give up and claim failure due to deadlock prevention. */
		if (pLeftChild == NULL && pChild->pLeftSibling != NULL)
		{
			if (RWLockWriteTryLock("BPMergeOrStealChildKeyNodes-LeftChildUnderDiffParent", &(pChild->pLeftSibling->rwNodeLock), 5))
			{
				pLeftChild = pChild->pLeftSibling;
			}
			else
			{
				/* We need to fail, which means we need to release the right lock and return an error */
				rc = BP_RC_Deadlock_Prevention;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes->rightchild", &pRightChild->rwNodeLock);
			}
		}

		if (rc == BP_RC_Success)
		{
			/* We will merge the child into the right node. */
			/* Move the contents of the right node all the way down! */
			memmove(&(pRightChild->ppDataPtrArray[pChild->llNumInNode + 1]), &(pRightChild->ppDataPtrArray[0]), pRightChild->llNumInNode * sizeof(char*));
			memmove(&(pRightChild->ppChildArray[pChild->llNumInNode + 1]), &(pRightChild->ppChildArray[0]), (pRightChild->llNumInNode + 1) * sizeof(PBPNode));

			/* Now copy the parent's key into the llNumInNode position */
			pRightChild->ppDataPtrArray[pChild->llNumInNode] = pParent->ppDataPtrArray[childTraversalIdx];
			/* Move the contents of the parent down. */
			memmove(&(pParent->ppDataPtrArray[childTraversalIdx]), &(pParent->ppDataPtrArray[childTraversalIdx + 1]), ((pParent->llNumInNode - childTraversalIdx) - 1) * sizeof(char*));
			/* We need to keep the right child's ptr in the parent and destroy the child's ptr ... so copy stuff down there as well */
			memmove(&(pParent->ppChildArray[childTraversalIdx]), &(pParent->ppChildArray[childTraversalIdx + 1]), ((pParent->llNumInNode - childTraversalIdx)) * sizeof(PBPNode));

			(pParent->llNumInNode)--;

			/* Now copy all the left junk into the right junk */
			memcpy(&(pRightChild->ppDataPtrArray[0]), &(pChild->ppDataPtrArray[0]), (pChild->llNumInNode) * sizeof(char*));
			memcpy(&(pRightChild->ppChildArray[0]), &(pChild->ppChildArray[0]), (pChild->llNumInNode + 1) * sizeof(PBPNode));

			/* Now we gotta fix parentage of all the child ptrs copied */
			for (BPLL idx = 0; idx < (pChild->llNumInNode + 1); idx++)
			{
				RWLockWriteLock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pRightChild->ppChildArray[idx]->rwNodeLock));
				pRightChild->ppChildArray[idx]->pParent = pRightChild;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-childchild for parentage", &(pRightChild->ppChildArray[idx]->rwNodeLock));
			}

			(pRightChild->llNumInNode) += (pChild->llNumInNode + 1);

			pChild->llNumInNode = 0;
			pRightChild->pLeftSibling = pLeftChild;

			if (pLeftChild != NULL)
			{
				pLeftChild->pRightSibling = pRightChild;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes-leftChild", &pLeftChild->rwNodeLock);
			}

			RWLockWriteUnlock("BPMergeOrStealChildKeyNodes - child for removal", &pChild->rwNodeLock);
			BPFreeNode(pTree, pIdxInfo, pChild);

#ifdef DEBUG_ON
			if (IsDebugging(DEBUG_DELETE))
			{
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: After the right merge!\n");
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The parent (0x%016p) is as follows:\n", pParent);
				BPPrintNode(fpDebug, pTree, pParent);
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
				BPPrintNode(fpDebug, pTree, pLeftChild);
				fprintf(fpDebug, "BPMergeOrStealChildKeyNodes: The right child (0x%016p) is as follows:\n", pRightChild);
				BPPrintNode(fpDebug, pTree, pRightChild);
				fflush(fpDebug);
			}
#endif

			* ppChild = pRightChild;
		}
	}

	return rc;
}
BPRc BPMergeOrStealChildLeafNodes(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pParent, BPLL childTraversalIdx, PBPNode pLeftChild, PBPNode* ppChild, PBPNode pRightChild, bool freeData)
{
	PBPNode pChild = *ppChild;
	BPRc rc = BP_RC_Success;

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: Entered\n");
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The childTraversalIdx is %zd\n", childTraversalIdx);
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The parent (0x%016p) is as follows:\n", pParent);
		BPPrintNode(fpDebug, pTree, pParent);
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
		BPPrintNode(fpDebug, pTree, pLeftChild);
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The child (0x%016p) is as follows:\n", pChild);
		BPPrintNode(fpDebug, pTree, pChild);
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The right child (0x%016p) is as follows:\n", pRightChild);
		BPPrintNode(fpDebug, pTree, pRightChild);
		fflush(fpDebug);
	}
#endif

	/* Prefer to steal from right first */
	if (pRightChild != NULL && pRightChild->llNumInNode > pTree->llMinKeys)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: We are stealing from the right node!\n");
			fflush(fpDebug);
		}
#endif

		/* First, add the right's first key value to the child. */
		pChild->ppDataPtrArray[pChild->llNumInNode] = pRightChild->ppDataPtrArray[0];
		/* Increase the child's count */
		(pChild->llNumInNode)++;
		/* Now copy down the contents of the right child */
		memmove(&(pRightChild->ppDataPtrArray[0]), &(pRightChild->ppDataPtrArray[1]), (pRightChild->llNumInNode - 1) * sizeof(char*));
		/* Remove one from the right child */
		(pRightChild->llNumInNode)--;

		/* Change out the parent's key since the first item changed in the right node. */
		pParent->ppDataPtrArray[childTraversalIdx] = pRightChild->ppDataPtrArray[0];

		/* Free the left and right locks */
		if (pLeftChild != NULL)
			RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-leftChild", &pLeftChild->rwNodeLock);
		RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-RightChild", &pRightChild->rwNodeLock);

#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: After the right steal!\n");
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The parent (0x%016p) is as follows:\n", pParent);
			BPPrintNode(fpDebug, pTree, pParent);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
			BPPrintNode(fpDebug, pTree, pLeftChild);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The child (0x%016p) is as follows:\n", pChild);
			BPPrintNode(fpDebug, pTree, pChild);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The right child (0x%016p) is as follows:\n", pRightChild);
			BPPrintNode(fpDebug, pTree, pRightChild);
			fflush(fpDebug);
		}
#endif


	}
	else if (pLeftChild != NULL && pLeftChild->llNumInNode > pTree->llMinKeys)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: We are stealing from the left node!\n");
			fflush(fpDebug);
		}
#endif
		/* We gotta shift everything to the right in the child. */
		memmove(&(pChild->ppDataPtrArray[1]), &(pChild->ppDataPtrArray[0]), pChild->llNumInNode * sizeof(char*));
		/* Add the left's last key value to the child */
		pChild->ppDataPtrArray[0] = pLeftChild->ppDataPtrArray[pLeftChild->llNumInNode - 1];
		/* Increase the child's count */
		(pChild->llNumInNode)++;
		/* Remove one from the left  child */
		(pLeftChild->llNumInNode)--;

		/* Change out the parent's key since the first item changed in the child node */
		pParent->ppDataPtrArray[childTraversalIdx - 1] = pChild->ppDataPtrArray[0];

		/* Free the left and right locks */
		RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-leftChild", &pLeftChild->rwNodeLock);
		if(pRightChild != NULL)
			RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-RightChild", &pRightChild->rwNodeLock);

#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: After the left steal!\n");
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The parent (0x%016p) is as follows:\n", pParent);
			BPPrintNode(fpDebug, pTree, pParent);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
			BPPrintNode(fpDebug, pTree, pLeftChild);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The child (0x%016p) is as follows:\n", pChild);
			BPPrintNode(fpDebug, pTree, pChild);
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The right child (0x%016p) is as follows:\n", pRightChild);
			BPPrintNode(fpDebug, pTree, pRightChild);
			fflush(fpDebug);
		}
#endif


	}
	else if (pLeftChild != NULL)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: We are merging the child with the left child!\n");
			fflush(fpDebug);
		}
#endif

		/* We really need to lock the right child first so that we can update sibling linkage.  RightChild passed in might be null if the right sibling */
		/* is under a different parent.  We need to check this and try to obtain the lock.  If we cannot obtain the lock, give up and claim failure due to deadlock prevention. */
		if (pRightChild == NULL && pChild->pRightSibling != NULL)
		{
			if (RWLockWriteTryLock("BPMergeOrStealChildKeyNodes-RightChildUnderDiffParent", &(pChild->pRightSibling->rwNodeLock), 5))
			{
				pRightChild = pChild->pRightSibling;
			}
			else
			{
				/* We need to fail, which means we need to release the left lock and return an error */
				rc = BP_RC_Deadlock_Prevention;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes->leftchild", &pLeftChild->rwNodeLock);
			}
		}

		if (rc == BP_RC_Success)
		{

			/* First, copy all of the data from the child into the left node. */
			memcpy(&(pLeftChild->ppDataPtrArray[pLeftChild->llNumInNode]), &(pChild->ppDataPtrArray[0]), pChild->llNumInNode * sizeof(char*));
			/* Increase the size of the left child */
			(pLeftChild->llNumInNode) += pChild->llNumInNode;
			/* Zero out the child in preparation for freeing */
			pChild->llNumInNode = 0;

			/* Hook up the left child to the right child */
			pLeftChild->pRightSibling = pRightChild;
			if (pRightChild != NULL)
				pRightChild->pLeftSibling = pLeftChild;

			/* Remove the child from the parent */
			memmove(&(pParent->ppDataPtrArray[childTraversalIdx - 1]), &(pParent->ppDataPtrArray[childTraversalIdx]), (pParent->llNumInNode - childTraversalIdx) * sizeof(char*));
			memmove(&(pParent->ppChildArray[childTraversalIdx]), &(pParent->ppChildArray[childTraversalIdx + 1]), (pParent->llNumInNode - childTraversalIdx) * sizeof(PBPNode));

			/* Decrement the parent count */
			(pParent->llNumInNode)--;

			/* Unlock the child and free it */
			RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-child old", &pChild->rwNodeLock);
			BPFreeNodeButNotData(pTree, pIdxInfo, pChild);

			/* Unlock the right child if there */
			if (pRightChild != NULL)
				RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-RightChild", &pRightChild->rwNodeLock);

#ifdef DEBUG_ON
			if (IsDebugging(DEBUG_DELETE))
			{
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: After the left merge!\n");
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The parent (0x%016p) is as follows:\n", pParent);
				BPPrintNode(fpDebug, pTree, pParent);
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
				BPPrintNode(fpDebug, pTree, pLeftChild);
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The right child (0x%016p) is as follows:\n", pRightChild);
				BPPrintNode(fpDebug, pTree, pRightChild);
				fflush(fpDebug);
			}
#endif

			* ppChild = pLeftChild;
		}
	}
	else
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: We are merging the child with the right child!\n");
			fflush(fpDebug);
		}
#endif
		/* We really need to lock the left child first so that we can update sibling linkage.  LeftChild passed in might be null if the left sibling */
        /* is under a different parent.  We need to check this and try to obtain the lock.  If we cannot obtain the lock, give up and claim failure due to deadlock prevention. */
		if (pLeftChild == NULL && pChild->pLeftSibling != NULL)
		{
			if (RWLockWriteTryLock("BPMergeOrStealChildKeyNodes-LeftChildUnderDiffParent", &(pChild->pLeftSibling->rwNodeLock), 5))
			{
				pLeftChild = pChild->pLeftSibling;
			}
			else
			{
				/* We need to fail, which means we need to release the right lock and return an error */
				rc = BP_RC_Deadlock_Prevention;
				RWLockWriteUnlock("BPMergeOrStealChildKeyNodes->rightchild", &pRightChild->rwNodeLock);
			}
		}

		if (rc == BP_RC_Success)
		{
			/* We will merge the child into the right node. */
			/* Move the contents of the right node all the way down! */
			memmove(&(pRightChild->ppDataPtrArray[pChild->llNumInNode]), &(pRightChild->ppDataPtrArray[0]), pRightChild->llNumInNode * sizeof(char*));
			/* Now copy all the left junk into the right junk */
			memcpy(&(pRightChild->ppDataPtrArray[0]), &(pChild->ppDataPtrArray[0]), (pChild->llNumInNode) * sizeof(char*));
			/* Bump the size of the right child */
			(pRightChild->llNumInNode) += pChild->llNumInNode;
			/* Zero out the child in preparation for freeing */
			pChild->llNumInNode = 0;

			/* Hook up the left child to the right child */
			pRightChild->pLeftSibling = pLeftChild;
			if (pLeftChild != NULL)
				pLeftChild->pRightSibling = pRightChild;

			/* Remove the child from the parent */
			memmove(&(pParent->ppDataPtrArray[childTraversalIdx]), &(pParent->ppDataPtrArray[childTraversalIdx + 1]), ((pParent->llNumInNode - childTraversalIdx) - 1) * sizeof(char*));
			memmove(&(pParent->ppChildArray[childTraversalIdx]), &(pParent->ppChildArray[childTraversalIdx + 1]), (pParent->llNumInNode - childTraversalIdx) * sizeof(PBPNode));

			/* Decrement the parent count */
			(pParent->llNumInNode)--;

			/* Unlock the left node if there */
			if (pLeftChild != NULL)
				RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-LeftChild", &pLeftChild->rwNodeLock);

			/* Unlock the child and free it */
			RWLockWriteUnlock("BPMergeOrStealChildLeafNodes-child old", &pChild->rwNodeLock);
			BPFreeNodeButNotData(pTree, pIdxInfo, pChild);

#ifdef DEBUG_ON
			if (IsDebugging(DEBUG_DELETE))
			{
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: After the right merge!\n");
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The parent (0x%016p) is as follows:\n", pParent);
				BPPrintNode(fpDebug, pTree, pParent);
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The left child (0x%016p) is as follows:\n", pLeftChild);
				BPPrintNode(fpDebug, pTree, pLeftChild);
				fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: The right child (0x%016p) is as follows:\n", pRightChild);
				BPPrintNode(fpDebug, pTree, pRightChild);
				fflush(fpDebug);
			}
#endif

			* ppChild = pRightChild;
		}
	}

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPMergeOrStealChildLeafNodes: Exiting\n");
		fflush(fpDebug);
	}
#endif

	return rc;
}

/*
** BPDeleteProactiveRebalance — Recursively descend toward the delete target, proactively
** stealing from or merging with a sibling before entering each underfull child node.
** This guarantees we never need to backtrack upward for rebalancing: by the time we reach
** the leaf, every node on the path already satisfies the minimum-fill invariant.
*/
BPRc BPDeleteProactiveRebalance(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode, bool treeLockHeld, void* pData, BPLL idxOfInterest, PBPNode pFoundKeyNode, BPLL foundKeyNodeIdxOfInterest, bool freeData)
{
	BPLL childTraversalIdx;
	BPRc rc = BP_RC_Success;

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPDeleteProactiveRebalance: Entered!\n");
		fprintf(fpDebug, "BPDeleteProactiveRebalance: treeLockHeld = %s\n", treeLockHeld ? "true" : "false");
		fprintf(fpDebug, "BPDeleteProactiveRebalance: pData (0x%016p): '", pData);
		BPPrintKeyAtAddress(fpDebug, pTree, pData);
		fprintf(fpDebug, "'\n");
		fprintf(fpDebug, "BPDeleteProactiveRebalance: idxOfInterest = %zd\n", idxOfInterest);
		fprintf(fpDebug, "BPDeleteProactiveRebalance: pFoundKeyNode = 0x%016p\n", pFoundKeyNode);
		fprintf(fpDebug, "BPDeleteProactiveRebalance: foundKeyNodeIdxOfInterest = %zd\n", foundKeyNodeIdxOfInterest);
		fprintf(fpDebug, "BPDeleteProactiveRebalance: freeData = %s\n", freeData ? "true" : "false");
		fprintf(fpDebug, "BPDeleteProactiveRebalance: The node is as follows:\n");
		BPPrintNode(fpDebug, pTree, pNode);
		fflush(fpDebug);

	}
#endif

	/* If the index is positive, it means the key value was found in this key node.  The actual "traversal" child idx is +1 of that value. */
	/* If the index is negative, it simply means we need to negate it and subtract one from that value to get our traversal child idx value  */
	if (idxOfInterest < 0)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteProactiveRebalance: idxOfInterest was negative (not directly found)\n");
			fflush(fpDebug);
		}
#endif
		childTraversalIdx = (-idxOfInterest) - 1;
	}
	else
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteProactiveRebalance: idxOfInterest was positive (directly found)\n");
			fflush(fpDebug);
		}
#endif
		/* This also means we found the key node that has the value (which also means it's in a 0th position in a leaf somewhere) */
		pFoundKeyNode = pNode;
		foundKeyNodeIdxOfInterest = idxOfInterest;
		childTraversalIdx = idxOfInterest + 1;
	}

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPDeleteProactiveRebalance: resulting childTraversalIdx = %zd\n", childTraversalIdx);
		fflush(fpDebug);
	}
#endif


	PBPNode pLeftChild = NULL;
	PBPNode pRightChild = NULL;
	PBPNode pChild = NULL;

	/* Ok, we happen to know this is the root node, but we're going to go through the motions anyway.  We could shortcut but if we do, then I won't know if I can use the same routine as any other key */
	/* We need to lock 3 children nodes: left of the childTraversalIdx, the childTraversalIdx, and right of the child traversal idx. */
	/* We have to lock them all in case we need to steal or merge.  And we don't know which one.  We MUST keep locking in the down->left to right order or we will deadlock. So we must lock them all. */

	if (childTraversalIdx > 0)
	{
		pLeftChild = pNode->ppChildArray[childTraversalIdx - 1];
		/* Lock it */
		RWLockWriteLock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-leftChild", &pLeftChild->rwNodeLock);
	}

	pChild = pNode->ppChildArray[childTraversalIdx];
	RWLockWriteLock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-child", &pChild->rwNodeLock);

	if (childTraversalIdx < pNode->llNumInNode)
	{
		pRightChild = pNode->ppChildArray[childTraversalIdx + 1];
		/* Lock it */
		RWLockWriteLock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-RightChild", &pRightChild->rwNodeLock);
	}

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPDeleteProactiveRebalance: left child node address: 0x%016p\n", pLeftChild);
		fprintf(fpDebug, "BPDeleteProactiveRebalance: child node address: 0x%016p\n", pChild);
		fprintf(fpDebug, "BPDeleteProactiveRebalance: right child node address: 0x%016p\n", pRightChild);
		fflush(fpDebug);
	}
#endif

	/* See if the child has enough in it.  If it does, we do nothing. */
	if (pChild->llNumInNode <= pTree->llMinKeys)
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteProactiveRebalance: child doesn't have enough to prevent underflow!\n");
			fflush(fpDebug);
		}
#endif

		/* Well we gotta steal or merge the children. The algo to do that is different from keys and leafs, so do that check.*/
		/* These routines will free the locks on ALL nodes except the parent (pNode) and the pChild. */
		if (BPIsKeyNode(pChild))
			rc = BPMergeOrStealChildKeyNodes(pTree, pIdxInfo, pNode, childTraversalIdx, pLeftChild, &pChild, pRightChild, freeData);
		else
			rc = BPMergeOrStealChildLeafNodes(pTree, pIdxInfo, pNode, childTraversalIdx, pLeftChild, &pChild, pRightChild, freeData);

		if (rc != BP_RC_Success)
		{
			/* Free the node */
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pChild", &pChild->rwNodeLock);

			/* If this was the root node, we also held onto the tree lock.  We gotta free that first. */
			if (treeLockHeld)
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pTree", &pTree->rwTreeLock);
			/* Free the node */
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pNode", &pNode->rwNodeLock);
			return rc;
		}

		/* We need to nuke the pFound if it is this node.  If it isn't ... don't nuke it. */
		if (pFoundKeyNode == pNode)
		{
			if (foundKeyNodeIdxOfInterest < pNode->llNumInNode)
			{
				/* Recheck to make sure the idxOfInterest contains the pData value.  If it does, we keep the setting otherwise we nuke it */
				if (BPKeyCmpPP(pTree, pIdxInfo, pNode->ppDataPtrArray[foundKeyNodeIdxOfInterest], pData) != 0)
				{
					pFoundKeyNode = NULL;
					foundKeyNodeIdxOfInterest = -1;
				}
			}
			else
			{
				pFoundKeyNode = NULL;
				foundKeyNodeIdxOfInterest = -1;
			}
		}
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteProactiveRebalance: After underflow processing the found info is: \n");
			fprintf(fpDebug, "BPDeleteProactiveRebalance: pFoundKeyNode = 0x%016p\n", pFoundKeyNode);
			fprintf(fpDebug, "BPDeleteProactiveRebalance: foundKeyNodeIdxOfInterest = %zd\n", foundKeyNodeIdxOfInterest);
			fflush(fpDebug);

		}
#endif

		/* Since this is the root node, (we can check that but don't need to) if the llNumInNode is zero, we nuke the root node */
		if (pNode->llNumInNode == 0)
		{
#ifdef DEBUG_ON
			if (IsDebugging(DEBUG_DELETE))
			{
				fprintf(fpDebug, "BPDeleteProactiveRebalance: It was the root node and now we need to remove a level!\n");
				fflush(fpDebug);
			}
#endif
			if (treeLockHeld)
			{
				pChild->pParent = NULL;
				/* Lock the idx and set it up */
				RWLockWriteLock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-idx", &pIdxInfo->rwIdxLock);
				pIdxInfo->pRootNode = pChild;
				(pIdxInfo->stIdxDepth)--;
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-idx", &pIdxInfo->rwIdxLock);

				/* Now free the parent node (pNode) */
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-root (old)", &pNode->rwNodeLock);
				BPFreeNode(pTree, pIdxInfo, pNode);

				/* Since we know that the pChild is the new root, we gotta unlock the tree lock.*/
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-tree", &pTree->rwTreeLock);
			}
			else
			{
				Fatal(FATAL_BP_NODE_EMPTY, "We blew it.  Somehow the node is zero but it isn't the root????\n");
			}
		}
		else
		{
			/* If this was the root node, we also held onto the tree lock.  We gotta free that first. */
			if (treeLockHeld)
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pTree", &pTree->rwTreeLock);

			/* Only free the node if it isn't the found node! */
			if(pFoundKeyNode != pNode)
				RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pNode", &pNode->rwNodeLock);
		}
	}
	else
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteProactiveRebalance: The child will not underflow!\n");
			fflush(fpDebug);
		}
#endif
		/* Just free up the left and right child locks and this node since we don't need it anymore */
		if(pLeftChild != NULL)
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-leftChild", &pLeftChild->rwNodeLock);
		if (pRightChild != NULL)
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-rightChild", &pRightChild->rwNodeLock);
		/* If this was the root node, we also held onto the tree lock.  We gotta free that first. */
		if (treeLockHeld)
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pTree", &pTree->rwTreeLock);
		/* Only free the node if it isn't the found node! */
		if (pFoundKeyNode != pNode)
			RWLockWriteUnlock("BPCheckChildrenInRootKeyNodeForUnderflowBeforeFindingLeafToRemoveItFrom-pNode", &pNode->rwNodeLock);
	}


	if (rc == BP_RC_Success)
	{
		BPLL idxToTraverse = BPFindNodeDataBinary(pTree, pIdxInfo, pChild, pData);

		/* At this pt, we just have the child lock (and the child COULD be the new root. Regardless, it won't underflow. */
		if (BPIsLeafNode(pChild))
		{
			rc = BPRemoveFromNonUnderflowLeaf(pTree, pIdxInfo, pChild, pData, idxToTraverse, pFoundKeyNode, foundKeyNodeIdxOfInterest, freeData);
		}
		else
		{
			rc = BPDeleteProactiveRebalance(pTree, pIdxInfo, pChild, false, pData, idxToTraverse, pFoundKeyNode, foundKeyNodeIdxOfInterest, freeData);
		}
	}

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPDeleteProactiveRebalance: Exiting!\n");
		fflush(fpDebug);
	}
#endif


	return rc;

}


static BPRc BPDeleteDataItemOnce(PBPTree pTree, void* pData, bool freeData)
{
	BPRc rc = BP_RC_Success;

#ifdef DEBUG_ON
	if (IsDebugging(DEBUG_DELETE))
	{
		fprintf(fpDebug, "BPDeleteDataItem2: Request to delete and %sfree the key '", freeData ? "" : "not ");
		BPPrintKeyAtAddress(fpDebug, pTree, pData);
		fprintf(fpDebug, "'\n");
		fflush(fpDebug);
	}
#endif

	/* Grab the lock on the tree */
	RWLockWriteLock("BPDeleteDataItem-tree", &pTree->rwTreeLock);

	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	RWLockReadLock("BPDeleteDataItem-idx", &pIdxInfo->rwIdxLock);
	PBPNode pRoot = pTree->keyInfo.pRootNode;
	RWLockReadUnlock("BPDeleteDataItem-idx", &pIdxInfo->rwIdxLock);

	/* Now lock the root ptr in the index */
	RWLockWriteLock("BPDeleteDataItem-root", &pRoot->rwNodeLock);

	/* Now find the node we need to remove from or traverse into */
	BPLL idxToTraverse = BPFindNodeDataBinary(pTree, pIdxInfo, pRoot, pData);

	/* Easy case, root is a single leaf node, we simply nuke it if we find it */
	if (BPIsLeafNode(pRoot))
	{
#ifdef DEBUG_ON
		if (IsDebugging(DEBUG_DELETE))
		{
			fprintf(fpDebug, "BPDeleteDataItem2: The root is a leaf so doing a simple find/delete\n");
			fflush(fpDebug);
		}
#endif

		rc = BPRemoveFromNonUnderflowLeaf(pTree, pIdxInfo, pRoot, pData, idxToTraverse, NULL, -1, freeData);
		RWLockWriteUnlock("BPDeleteDataItem-tree", &pTree->rwTreeLock);
	}
	else
	{
		rc = BPDeleteProactiveRebalance(pTree, pIdxInfo, pRoot, true, pData, idxToTraverse, NULL, -1, freeData);
	}
	return rc;
}

BPRc BPDeleteDataItem(PBPTree pTree, void* pData, bool freeData)
{
	/* A cross-parent merge may fail to acquire a sibling's write lock in time.
	** When that happens all locks are already released, so yield and retry. */
	const int MAX_RETRIES = 5;
	BPRc rc = BP_RC_Deadlock_Prevention;
	for (int attempt = 0; attempt < MAX_RETRIES && rc == BP_RC_Deadlock_Prevention; attempt++)
	{
		if (attempt > 0)
			std::this_thread::yield();
		rc = BPDeleteDataItemOnce(pTree, pData, freeData);
	}
	return rc;
}

BPRc BPDeleteDataNoFree(PBPTree pTree, void* pData)
{
	return BPDeleteDataItem(pTree, pData, false);
}

BPRc BPDeleteDataAndFree(PBPTree pTree, void* pData)
{
	return BPDeleteDataItem(pTree, pData, true);
}
