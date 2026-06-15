#include "BP.h"
#include <stdlib.h>

//#define BPTREE_VERBOSE_CHECK


bool BPTreeCheckDepthForIdx(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;
	size_t level = 1;
	PBPNode pNode = pIdx->pRootNode;

	while (BPIsKeyNode(pNode))
	{
		pNode = pNode->ppChildArray[0];
		level++;
	}

	if (level != pIdx->stIdxDepth)
	{
		fprintf(fpOut, "***** INDEX DEPTH CORRUPT! ******\n");
		fprintf(fpOut, "The level noted in the index is: %zd  But it has been calculated to be: %zd\n", pIdx->stIdxDepth, level);
		result = false;
	}

	return result;
}
bool BPTreeCheckLeafDataOrderByIdx(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;

	PBPNode pLeafNode = pIdx->pRootNode;

	while (BPIsKeyNode(pLeafNode))
	{
		pLeafNode = pLeafNode->ppChildArray[0];
	}

	PBPNode pPrevLeafNode = NULL;

	char* prevDataPtr = pLeafNode->ppDataPtrArray[0];
	BPLL llNextIdx = 1;

	/* Now that we found it we loop for each sibling */
	while (pLeafNode != NULL)
	{
		if (pLeafNode->pLeftSibling != pPrevLeafNode)
		{
			fprintf(fpOut, "****** WARNING: LeftSibling in leaf node 0x%p is invalid!  It should be 0x%p!\n", pLeafNode->pLeftSibling, pPrevLeafNode);
			result = false;
		}
		pPrevLeafNode = pLeafNode;

		while (llNextIdx < pLeafNode->llNumInNode)
		{
			if (BPKeyCmpPP(pTree, &(pTree->keyInfo), prevDataPtr, pLeafNode->ppDataPtrArray[llNextIdx]) < 0)
			{
				prevDataPtr = pLeafNode->ppDataPtrArray[llNextIdx];
				llNextIdx++;
			}
			else
			{
				fprintf(fpOut, "***** DATA SORT ORDER CORRUPT! ******\n");
				fprintf(fpOut, "Look near node: 0x%p\n", pLeafNode);
				fprintf(fpOut, "Key1: '");
				BPPrintKeyAtAddress(fpOut, pTree, prevDataPtr);
				fprintf(fpOut, "'\n");

				fprintf(fpOut, "Key2: '");
				BPPrintKeyAtAddress(fpOut, pTree, pLeafNode->ppDataPtrArray[llNextIdx]);
				fprintf(fpOut, "'\n");

				fflush(fpOut);
				result = false;
				return result;
			}
		}
		llNextIdx = 0;
		pLeafNode = pLeafNode->pRightSibling;
	}

	return result;

}

typedef struct _TheDataList
{
	void *dataPtr;
	_TheDataList* pNxtInList;
} ListNode, * PListNode;


bool BPTreeCheckKeyNodesAddToList(FILE* fpOut, void *thePtr, PListNode* ppListPtr)
{
	bool errFound = false;

	while ((*ppListPtr) != NULL && !errFound)
	{
		if (thePtr < (*ppListPtr)->dataPtr)
			break;
		else if (thePtr == (*ppListPtr)->dataPtr)
			errFound = true;
		else
			ppListPtr = &((*ppListPtr)->pNxtInList);
	}

	if (!errFound)
	{
		PListNode pNew = (PListNode)MemMalloc("ListNodeForCheck", sizeof(ListNode));
		if (pNew == NULL)
		{
			fprintf(fpOut, "Could not allocate memory for BPTreeCheckKeyNodesAddToList\n");
			errFound = true;
		}
		else
		{

			pNew->dataPtr = thePtr;
			pNew->pNxtInList = NULL;

			if (*ppListPtr != NULL)
			{
				pNew->pNxtInList = *ppListPtr;
			}
			*ppListPtr = pNew;
		}
	}

	return errFound;

}

void BPTreeCheckKeyFreeListNode(PListNode pTopNode)
{
	PListNode pCurr = pTopNode;

	while (pCurr != NULL)
	{
		pTopNode = pCurr->pNxtInList;
		MemFree(pCurr);
		pCurr = pTopNode;
	}

}
bool BPTreeCheckRootNode(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;
	/* May be a key node but we aren't gonna check the ptrs so no big deal */
	PBPNode pNode = pIdx->pRootNode;

	if (pNode->pLeftSibling != NULL)
	{
		fprintf(fpOut, "The root node 0x%p should not have a left sibling but does (0x%p)!\n", pNode, pNode->pLeftSibling);
		result = false;
	}

	if (pNode->pRightSibling != NULL)
	{
		fprintf(fpOut, "The root node 0x%p should not have a right sibling but does (0x%p)!\n", pNode, pNode->pRightSibling);
		result = false;
	}

	if (pNode->pParent != NULL)
	{
		fprintf(fpOut, "The root node 0x%p should not have a parent but does (0x%p)!\n", pNode, pNode->pParent);
		result = false;
	}

	return result;
}

bool BPTreeCheckKeyNodes(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;
	PListNode pTopNode = NULL;
	size_t    level = 1;
	size_t    numKeyNodes = 0;
	PBPNode   pLeftMostKeyNode = pIdx->pRootNode;
	PBPNode   pPrevNode = NULL;
	PBPNode   pCurr;

	while (BPIsKeyNode(pLeftMostKeyNode))
	{
#ifdef BPTREE_VERBOSE_CHECK
		fprintf(fpOut, "Checking Key Node Level: %zu\n", level);
#endif
		pPrevNode = NULL;
		pCurr = pLeftMostKeyNode;

		while (pCurr != NULL)
		{
			if (pCurr->pLeftSibling != pPrevNode)
			{
				fprintf(fpOut, "LeftSiblingOffset Is Corrupt in node: 0x%p (level %zd)!  Should be 0x%p but is 0x%p!\n", pCurr, level, pPrevNode, pCurr->pLeftSibling);
				result = false;
			}
			pPrevNode = pCurr;
			numKeyNodes++;

#ifdef BPTREE_VERBOSE_CHECK
			if(numKeyNodes % 100 == 0)
				fprintf(fpOut, "Checking KeyNode: %zu\r", numKeyNodes);
#endif

			for (BPLL idx = 0; idx < pCurr->llNumInNode; idx++)
			{
				if (BPTreeCheckKeyNodesAddToList(fpOut, pCurr->ppDataPtrArray[idx], &pTopNode))
				{
					fprintf(fpOut, "***** There is a data ptr that appears twice in the tree: 0x%p (see node 0x%p)\n", pCurr->ppDataPtrArray[idx], pCurr);
					result = false;
				}
			}
			pCurr = pCurr->pRightSibling;
		}

		pCurr = pLeftMostKeyNode->ppChildArray[0];
		pLeftMostKeyNode = pCurr;
		level++;
	}

	BPTreeCheckKeyFreeListNode(pTopNode);
	return result;
}

bool BPTreeCheckKeysExist(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;
	PBPNode pLeftMostKeyNode = pIdx->pRootNode;
	PBPNode	pCurr = pLeftMostKeyNode;
	void* pFoundData = MemMalloc("BPTreeCheckKeyExist.founduserdata", pTree->stDataSize);

	if (pFoundData == NULL)
	{
		fprintf(fpOut, "***** Could not allocate memory for BPTreeCheckKeysExist\n");
		return false;
	}

	while (BPIsKeyNode(pLeftMostKeyNode))
	{
		while (pCurr != NULL)
		{
			for (BPLL idx = 0; idx < pCurr->llNumInNode; idx++)
			{
				BPRc rc = BPFindEqualKey(pTree, pCurr->ppDataPtrArray[idx], pFoundData);

				if (rc != BP_RC_Success)
				{
					fprintf(fpOut, "***** There is a key that doesn't exist in the data! (can't be found by search!)(data: 0x%p keynode: 0x%p): ", pCurr->ppDataPtrArray[idx], pCurr);
					BPPrintKeyAtAddress(fpOut, pTree, pCurr->ppDataPtrArray[idx]);
					fprintf(fpOut, "\n");
					result = false;
				}
			}
			pCurr = pCurr->pRightSibling;
		}

		pCurr = pLeftMostKeyNode->ppChildArray[0];
		pLeftMostKeyNode = pCurr;
	}

	MemFree(pFoundData);

	return result;
}

bool BPTreeCheckAllDataFound(FILE* fpOut, PBPTree pTree, PBPIdxInfo pIdx)
{
	bool result = true;
	PBPNode pLeaf = pIdx->pRootNode;
	void* pFoundData = MemMalloc("BPTreeCheckKeyExist.founduserdata", pTree->stDataSize);

	if (pFoundData == NULL)
	{
		fprintf(fpOut, "***** Could not allocate memory for BPTreeCheckAllDataFound\n");
		return false;
	}

	while (pLeaf != NULL)
	{
		for (BPLL idx = 0; idx < pLeaf->llNumInNode; idx++)
		{
			BPRc rc = BPFindEqualKey(pTree, pLeaf->ppDataPtrArray[idx], pFoundData);

			if (rc != BP_RC_Success)
			{
				fprintf(fpOut, "***** There is a data item that doesn't exist in the data! (can't be found by search!)(data: 0x%p keynode: 0x%p): ", pLeaf->ppDataPtrArray[idx], pLeaf);
				BPPrintKeyAtAddress(fpOut, pTree, pLeaf->ppDataPtrArray[idx]);
				fprintf(fpOut, "\n");
				result = false;
			}
		}
		pLeaf = pLeaf->pRightSibling;
	}

	MemFree(pFoundData);
	return result;
}

bool BPCheckParentageForNode(FILE* fpOut, PBPTree pTree, PBPNode pNode, PBPNode pParent)
{
	bool result = true;

	/* First, make sure this node's parent is correct */
	if (pNode->pParent != pParent)
	{
		fprintf(fpOut, "**** Parentage corrupt!   See parent node: 0x%p and child node 0x%p!\n", pParent, pNode);
		result = false;
	}

	/* Now if this node is not a leaf, make sure each of it's childptr nodes have valid parentage */
	if (BPIsKeyNode(pNode) && result)
	{
		for (BPLL llIdx = 0; (llIdx < pNode->llNumInNode + 1) && result; llIdx++)
		{
			result = BPCheckParentageForNode(fpOut, pTree, pNode->ppChildArray[llIdx], pNode);
		}
	}
	return result;
}

bool BPCheckParentageForKey(FILE* fpOut, PBPTree pTree)
{
	return BPCheckParentageForNode(fpOut, pTree, pTree->keyInfo.pRootNode, NULL);
}

bool BPIntegrityCheck(FILE* fpOut, PBPTree pTree)
{
	bool result = true;

	fprintf(fpOut, "****** Performing Integrity Checks!!! *******\n");

	fprintf(fpOut, ">>> BPTreeCheckRootNode\n");
	fflush(fpOut);
	/* Make sure the root node is healthy */
	result &= BPTreeCheckRootNode(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPTreeCheckDepthForIdx\n");
	fflush(fpOut);
	/* Make sure the tree depth is intact */
	result &= BPTreeCheckDepthForIdx(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPTreeCheckLeafDataOrderByIdx\n");
	fflush(fpOut);
	/* Make sure the data elements are in sorted order */
	result &= BPTreeCheckLeafDataOrderByIdx(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPTreeCheckKeyNodes\n");
	fflush(fpOut);
	/* Make sure the key node entries do not have more than one copy of a data item. */
	result &= BPTreeCheckKeyNodes(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPTreeCheckKeysExist\n");
	fflush(fpOut);
	/* Make sure the key is actually in the leaf node */
	result &= BPTreeCheckKeysExist(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPTreeCheckAllDataFound\n");
	fflush(fpOut);
	/* Make sure all data items can be found by search */
	result &= BPTreeCheckAllDataFound(fpOut, pTree, &(pTree->keyInfo));

	fprintf(fpOut, ">>> BPCheckParentageForKey\n");
	fflush(fpOut);
	/* Validate all the parentage */
	result &= BPCheckParentageForKey(fpOut, pTree);

	fprintf(fpOut, "****** Finished Performing Integrity Checks!!! (%s) *******\n", result ? "Passed" : "Failed");
	fflush(fpOut);
	/* Intentionally fatal on failure — change to just "return result" if callers need to handle failures gracefully */
	if (!result)
	{
		Fatal(FATAL_BP_INTEGRITY_CHECK_FAILED, "**** Integrity check failed!!!!\n");
	}
	return result;

}
