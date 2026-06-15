#include "BP.h"

BPRc BPUpdate(PBPTree pTree, void* pDataToUpdate)
{
	BPRc rc = BP_RC_Success;
	PBPIdxInfo pIdxInfo = &(pTree->keyInfo);

	/* First lock the tree */
	RWLockReadLock("BPUpdate", &pTree->rwTreeLock);
	RWLockReadLock("BPUpdate", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPUpdate", &pIdxInfo->rwIdxLock);

	if (BPIsKeyNode(pNode))
		RWLockReadLock("BPUpdate", &pNode->rwNodeLock);
	else
		RWLockWriteLock("BPUpdate", &pNode->rwNodeLock);

	RWLockReadUnlock("BPUpdate", &pTree->rwTreeLock);

	/* Find the node where the key is or where the key should go */
	while (BPIsKeyNode(pNode))
	{
		BPLL keyIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pDataToUpdate);
		if (keyIdx < 0)
			keyIdx = (-(keyIdx)-1);
		else
			keyIdx++;

		/* Yes - remember the ppChildArray contains 1 more than the max! */
		PBPNode pNewNode = pNode->ppChildArray[keyIdx];
		if (BPIsKeyNode(pNewNode))
			RWLockReadLock("BPUpdate", &pNewNode->rwNodeLock);
		else
			RWLockWriteLock("BPUpdate", &pNewNode->rwNodeLock);
		RWLockReadUnlock("BPUpdate", &pNode->rwNodeLock);
		pNode = pNewNode;
	}

	BPLL llFoundDataOffsetIdx = BPFindNodeDataBinary(pTree, pIdxInfo, pNode, pDataToUpdate);

	if (llFoundDataOffsetIdx >= 0)
	{
		/* !!!! WARNING !!!!
		** The key fields in pDataToUpdate MUST be identical to the key fields of the record already in the tree.
		** The caller located this record BY its key, so the key is already known to match — only non-key fields
		** should differ.  If any key field is changed here, the separator values stored in the parent key nodes
		** will no longer match the leaf data, silently corrupting the tree structure.  There is no check for
		** this condition.  Use BPDeleteDataAndFree + BPInsertCopy to change a key value.
		*/
		memcpy(pNode->ppDataPtrArray[llFoundDataOffsetIdx], pDataToUpdate, pTree->stDataSize);
	}
	else
	{
		rc = BP_RC_Not_Found;
	}
	RWLockWriteUnlock("BPUpdate", &pNode->rwNodeLock);


	return rc;
}
