#include "BP.h"

BPLL countSiblings(PBPNode pNode, BPLL earlyExitAt)
{
    BPLL count = 1;
    PBPNode pSibling = pNode->pRightSibling;
    while (pSibling != NULL && count < earlyExitAt)
    {
        count++;
        pSibling = pSibling->pRightSibling;
    }
    return count;
}

// BPGetLevelStartKeys — find natural partition boundaries within the tree.
//
// PURPOSE
//   Splits the tree into roughly equal key-space partitions by locating the internal
//   node level whose sibling count is just at or below targetNodeCount.  Each sibling
//   at that level becomes one partition; the caller submits one parallel write job per
//   partition.
//
// CRITICAL USAGE CONSTRAINT
//   This routine returns RAW POINTERS directly into the tree's node data (ppDataPtrArray
//   entries of leaf nodes).  No copies are made.  The tree MUST NOT be modified or freed
//   while the caller holds these pointers.  This function is only safe to call on a
//   snapshot tree that has been swapped out of active use — no concurrent inserts,
//   deletes, or merges may be running against the same tree instance.
//
// PARAMETERS
//   pTree           — the tree to partition
//   pIdxInfo        — index info for pTree (caller already has it; avoids re-fetch)
//   targetNodeCount — desired maximum number of partitions; the function finds the
//                     deepest level whose node count does not exceed this value
//   outKeys         — receives one raw pointer per partition boundary, each pointing
//                     to the first record of the leftmost leaf of each non-first
//                     partition node.  If outKeys has N entries on return, the caller
//                     creates N+1 jobs:
//                       job 0  : [NULL,         outKeys[0])
//                       job 1  : [outKeys[0],   outKeys[1])
//                       ...
//                       job N  : [outKeys[N-1], NULL / end)
//                     outKeys is empty when the whole tree fits in one partition.
//
// ALGORITHM
//   1. Walk the leftmost path (ppChildArray[0] at each level) with a hand-over-hand
//      read lock, counting siblings at each level.  Stop at the first level whose
//      sibling count exceeds targetNodeCount and use the level above it — the last
//      level where count <= targetNodeCount.  If no level exceeds the threshold, the
//      deepest key-node level reached is used.
//   2. For each non-first sibling at the chosen level, descend to its leftmost leaf
//      and push ppDataPtrArray[0].  We must descend because ppDataPtrArray[0] of a
//      key node is the separator between its first and second children, not the true
//      minimum key of the subtree.  The leftmost leaf's ppDataPtrArray[0] always is.
//
// LOCKING
//   The descent in step 1 follows the same hand-over-hand read-lock pattern as
//   BPIterateStart: child lock acquired before parent lock released, so exactly one
//   node lock is held at all times.  The sibling walk and leaf descents in step 2
//   run without node locks — safe because the tree is a frozen snapshot with no
//   concurrent writers.
BPRc BPGetLevelStartKeys(
    BPTree* pTree,
	PBPIdxInfo pIdxInfo,
    BPLL targetNodeCount,
    std::vector<void *> &outKeys)
{
	BPRc rc = BP_RC_Success;
	outKeys.clear();
	PBPNode pNodeOfInterest = NULL;

	/* First lock the tree */
	RWLockReadLock("BPGetLevelStartKeys", &pTree->rwTreeLock);
	RWLockReadLock("BPGetLevelStartKeys", &pIdxInfo->rwIdxLock);
	PBPNode pNode = pIdxInfo->pRootNode;
	RWLockReadUnlock("BPGetLevelStartKeys", &pIdxInfo->rwIdxLock);
	pNodeOfInterest = pNode;

	RWLockReadLock("BPGetLevelStartKeys", &pNode->rwNodeLock);
	RWLockReadUnlock("BPGetLevelStartKeys", &pTree->rwTreeLock);

	while (BPIsKeyNode(pNode))
	{
        BPLL siblingCount = countSiblings(pNode, targetNodeCount + 1);

		if (siblingCount > targetNodeCount)
			break;

        pNodeOfInterest = pNode;

        PBPNode pChild = pNode->ppChildArray[0];
        RWLockReadLock("BPGetLevelStartKeys", &pChild->rwNodeLock);
        RWLockReadUnlock("BPGetLevelStartKeys", &pNode->rwNodeLock);
        pNode = pChild;
	}

    /* pNode has its read lock held.
       pNodeOfInterest is the leftmost node at the target level.
       Skip the first node (its partition starts at NULL).
       For each remaining sibling, descend to the leftmost leaf to get the true minimum key —
       ppDataPtrArray[0] of a key node is a child separator, not the subtree minimum. */
    PBPNode pSibling = pNodeOfInterest->pRightSibling;
    while (pSibling != NULL)
    {
        PBPNode p = pSibling;
        while (BPIsKeyNode(p))
            p = p->ppChildArray[0];
        outKeys.push_back(p->ppDataPtrArray[0]);
        pSibling = pSibling->pRightSibling;
    }

	RWLockReadUnlock("BPGetLevelStartKeys", &pNode->rwNodeLock);
	return rc;
}
