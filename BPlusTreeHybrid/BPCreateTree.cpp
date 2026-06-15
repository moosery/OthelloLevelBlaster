#define BP_CREATE_TREE
#include "BP.h"

void BPFreeTree(PBPTree pTree, bool freeData)
{
    if (pTree != NULL)
    {
        PArenaMem pArena = pTree->pArena;
        PBPNode currNode;
        PBPNode pNextLeftMostNode = pTree->keyInfo.pRootNode;

        while (pNextLeftMostNode != NULL)
        {
            currNode = pNextLeftMostNode;
            if (BPIsKeyNode(pNextLeftMostNode))
                pNextLeftMostNode = (PBPNode)pNextLeftMostNode->ppChildArray[0];
            else
                pNextLeftMostNode = NULL;

            while (currNode != NULL)
            {
                PBPNode pTmp = currNode->pRightSibling;
                if (freeData)
                    BPFreeNode(pTree, &(pTree->keyInfo), currNode);
                else
                    BPFreeNodeButNotData(pTree, &(pTree->keyInfo), currNode);

                currNode = pTmp;
            }
        }

        RWLockFree("BPFreeTree-idx", &pTree->keyInfo.rwIdxLock);
        RWLockFree("BPFreeTree-tree", &pTree->rwTreeLock);
        if (pArena) ArenaMemReset(pArena);
        MemFree(pTree);
    }
}

/*
** Name: BPCreateTree
** Purpose:
**   To allocate and initialize the tree structure.
**   Pass pArena = nullptr for malloc mode; pass a valid arena for arena mode.
**   maxMemoryBytes is used only in malloc mode to cap the data count;
**   in arena mode the arena itself acts as the capacity limit.
*/
BPRc BPCreateTree(PBPTree* ppTree, BPLL llOrder, size_t maxMemoryBytes, size_t stIdxSettings, size_t stNumFlds, BPIdxFld idxFlds[], size_t stDataSize, PArenaMem pArena)
{
    BPRc result = BP_RC_Success;

    *ppTree = (PBPTree)MemMalloc("BPTree", sizeof(BPTree));

    if (*ppTree == NULL)
        return BP_RC_Allocate_Failed;

    (*ppTree)->pArena = pArena;

    RWLockInit("BPTree", "BPCreateTree", &((*ppTree)->rwTreeLock));
    RWLockInit("BPIdxInfo", "BPCreateTree", &((*ppTree)->keyInfo.rwIdxLock));

    /* Sorry, but we need uneven key entries to allow for delete to merge on the way down.  It's a performance thing - and the delete takes advantage of this fact. */
    if (llOrder % 2 == 1)
        llOrder++;

    (*ppTree)->llOrder = llOrder;
    (*ppTree)->llMinKeys = (llOrder - 1) / 2;
    (*ppTree)->stDataSize = stDataSize;

    PBPIdxInfo pKeyInfo = &((*ppTree)->keyInfo);

    /* Set up the key info */
    pKeyInfo->stIdxSettings = 0;
    pKeyInfo->stIdxDepth = 1;
    pKeyInfo->stNumKeyNodes = 0;
    pKeyInfo->stNumLeafNodes = 0;
    pKeyInfo->stDataCnt = 0;
    pKeyInfo->stMaxDataCnt = (pArena != nullptr)
        ? BP_IDX_MAX_DATA_DEFAULT
        : ((stDataSize > 0) ? (maxMemoryBytes / stDataSize) : maxMemoryBytes);

    /* Verify the settings.  DUPLICATES are NOT allowed in the key.  Keys must be unique!!! */
    size_t goodFlags = 0;

    if (stIdxSettings & BP_IDX_SETTING_SORT_DESC)
        goodFlags |= BP_IDX_SETTING_SORT_DESC;


    if (goodFlags != stIdxSettings)
    {
        BPFreeTree(*ppTree,false);
        return BP_RC_Invalid_Settings;
    }

    pKeyInfo->stIdxSettings = goodFlags;

    if (stNumFlds > 0 && stNumFlds <= BP_IDX_MAX_KEY_FLDS)
    {
        pKeyInfo->stNumFlds = stNumFlds;
    }
    else
    {
        BPFreeTree(*ppTree, false);
        return BP_RC_Invalid_Num_Fields;
    }

    /* Now verify the field definitions in the index */

    for (size_t idx = 0; idx < stNumFlds; idx++)
    {
        PBPIdxFld pIdxFld = &(idxFlds[idx]);
        PBPIdxFld pKeyFld = &(pKeyInfo->idxFlds[idx]);

        if (pIdxFld->stDataOffset + pIdxFld->stLength <= stDataSize)
            pKeyFld->stDataOffset = pIdxFld->stDataOffset;
        else
        {
            BPFreeTree(*ppTree,false);
            return BP_RC_Invalid_Data_Offset;
        }

        pKeyFld->stDataType = pIdxFld->stDataType;

        switch (pIdxFld->stDataType)
        {
            case BP_IDX_DATATYPE_BINARY:
            case BP_IDX_DATATYPE_BYTE:
            case BP_IDX_DATATYPE_CHAR:
                pKeyFld->stLength = pIdxFld->stLength;
                break;
            case BP_IDX_DATATYPE_SNUM_2BYTE:
            case BP_IDX_DATATYPE_UNUM_2BYTE:
                pKeyFld->stLength = 2;
                break;
            case BP_IDX_DATATYPE_SNUM_4BYTE:
            case BP_IDX_DATATYPE_UNUM_4BYTE:
                pKeyFld->stLength = 4;
                break;
            case BP_IDX_DATATYPE_SNUM_8BYTE:
            case BP_IDX_DATATYPE_UNUM_8BYTE:
                pKeyFld->stLength = 8;
                break;

            default:
            {
                BPFreeTree(*ppTree,true);
                return BP_RC_Idx_Datatype_Invalid;
            }
        }
    }

    pKeyInfo->stLeafNodeSize = sizeof(BPNode) + (sizeof(char*) * ((*ppTree)->llOrder - 1));
    pKeyInfo->stKeyNodeSize = pKeyInfo->stLeafNodeSize + (sizeof(PBPNode) * ((*ppTree)->llOrder));

    /* Grab a node for the root (this will in turn allocate the first block of nodes!) */
    result = BPAllocateNode(*ppTree, pKeyInfo, &((*ppTree)->keyInfo.pRootNode), BP_NODEINFO_LEAF);

    if (result != BP_RC_Success)
    {
        BPFreeTree(*ppTree,false);
        return result;
    }

    //BPPrintTreeHeader(stdout, *ppTree);

    return(result);
}
