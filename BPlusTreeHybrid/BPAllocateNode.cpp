#include "BP.h"

void BPFreeNodeButNotData(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode)
{
	if (pNode != NULL)
	{
		if (BPIsLeafNode(pNode))
			(pIdxInfo->stNumLeafNodes)--;
		else (pIdxInfo->stNumKeyNodes)--;

		RWLockFree("BPFreeNodeButNotData-node", &pNode->rwNodeLock);
		if (pTree->pArena) ArenaMemFree(pTree->pArena, pNode);
		else               MemFree(pNode);
	}
}

void BPFreeNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode)
{
	if (pNode != NULL)
	{
		if (BPIsLeafNode(pNode))
		{
			for (BPLL idx = 0; idx < pNode->llNumInNode; idx++)
			{
				if (pTree->pArena) ArenaMemFree(pTree->pArena, pNode->ppDataPtrArray[idx]);
				else               MemFree(pNode->ppDataPtrArray[idx]);
			}
			(pIdxInfo->stNumLeafNodes)--;
		}
		else (pIdxInfo->stNumKeyNodes)--;

		RWLockFree("BPFreeNode-node", &pNode->rwNodeLock);
		if (pTree->pArena) ArenaMemFree(pTree->pArena, pNode);
		else               MemFree(pNode);
	}
}

BPRc BPAllocateNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode* ppNode, size_t nodeType)
{
	size_t sizeToAlloc = (nodeType & BP_NODEINFO_KEY) ? pIdxInfo->stKeyNodeSize : pIdxInfo->stLeafNodeSize;
	PBPNode pNewNode = pTree->pArena
		? (PBPNode)ArenaMemMalloc(pTree->pArena, "BPNode", sizeToAlloc)
		: (PBPNode)MemMalloc("BPNode", sizeToAlloc);

	if (pNewNode != NULL)
	{
		RWLockInit("BPNode Lock","BPAllocateNode", & pNewNode->rwNodeLock);

		pNewNode->stNodeInfo = nodeType;
		pNewNode->llNumInNode = 0;
		pNewNode->pParent = NULL;
		pNewNode->pLeftSibling = NULL;
		pNewNode->pRightSibling = NULL;
		pNewNode->ppDataPtrArray = NULL;
		pNewNode->ppChildArray = NULL;

		char* pCharPtr = (char*)pNewNode;
		pCharPtr += sizeof(BPNode);
		pNewNode->ppDataPtrArray = (char **) pCharPtr;
		RWLockWriteLock("BPAllocateNode-idx", & pIdxInfo->rwIdxLock);
		if (BPIsKeyNode(pNewNode))
		{
			pCharPtr += (sizeof(char*) * (pTree->llOrder - 1));
			pNewNode->ppChildArray = (BPNode**)pCharPtr;
			(pIdxInfo->stNumKeyNodes)++;
		}
		else (pIdxInfo->stNumLeafNodes)++;

		RWLockWriteUnlock("BPAllocateNode-idx", &pIdxInfo->rwIdxLock);
		*ppNode = pNewNode;
		return BP_RC_Success;
	}

	return BP_RC_Allocate_Failed;
}
