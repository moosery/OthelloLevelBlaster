#include "BP.h"

void BPPrintKeyAtAddress(FILE* fpOut, PBPTree pTree, void* ptrDatav)
{
	char* ptrData = (char*)ptrDatav;

	/* Loop through each key field doing it's datatype's compare process until one returns non-zero */
	for (size_t idx = 0; idx < pTree->keyInfo.stNumFlds; idx++)
	{
		char* pFld = (char*)(ptrData + pTree->keyInfo.idxFlds[idx].stDataOffset);
		size_t fldLen = pTree->keyInfo.idxFlds[idx].stLength;

		if (idx > 0)
			fprintf(fpOut, ",");

		switch (pTree->keyInfo.idxFlds[idx].stDataType)
		{
		case BP_IDX_DATATYPE_CHAR:
			fprintf(fpOut, "%-*.*s", (int)fldLen, (int)fldLen, pFld);
			break;
		case BP_IDX_DATATYPE_BYTE:
		case BP_IDX_DATATYPE_BINARY:
			for (size_t tmpIdx = 0; tmpIdx < fldLen; tmpIdx++, pFld++)
			{
				unsigned int value = (unsigned int)(*((unsigned char*)pFld)) & 0xFF;
				fprintf(fpOut, "%02x", value);
			}
			break;
			/* If we didn't have INTEL's ENDIANness ... we coulda used memcmp darn it */
		case BP_IDX_DATATYPE_SNUM_2BYTE:
		{
			fprintf(fpOut, "%hd", *((short*)pFld));
		}
		break;
		case BP_IDX_DATATYPE_UNUM_2BYTE:
		{
			fprintf(fpOut, "%hu", *((unsigned short*)pFld));
		}
		break;
		case BP_IDX_DATATYPE_SNUM_4BYTE:
		{
			fprintf(fpOut, "%ld", *((long*)pFld));
		}
		break;
		case BP_IDX_DATATYPE_UNUM_4BYTE:
		{
			fprintf(fpOut, "%lu", *((unsigned long*)pFld));
		}
		break;
		case BP_IDX_DATATYPE_SNUM_8BYTE:
		{
			fprintf(fpOut, "%lld", *((BPLL*)pFld));
		}
		break;
		case BP_IDX_DATATYPE_UNUM_8BYTE:
		{
			fprintf(fpOut, "0x%016llx", *((unsigned long long*)pFld));
		}
		break;
		default:
			break;
		}

	}
}

void BPPrintIdxInfo(FILE* fpOut, const char* szIdxName, PBPIdxInfo pIdxInfo)
{
	fprintf(fpOut, "---------------------------------------------------------------------------\n");
	fprintf(fpOut, "                      %s Index Information\n", szIdxName);
	fprintf(fpOut, "---------------------------------------------------------------------------\n");
	fprintf(fpOut, "  stIdxSettings             : 0x%016zx\n", pIdxInfo->stIdxSettings);
	fprintf(fpOut, "  pRootNode                 : 0x%p\n", pIdxInfo->pRootNode);
	fprintf(fpOut, "  stNumFlds                 : %zu\n", pIdxInfo->stNumFlds);
	fprintf(fpOut, "  stNumKeyNodes             : %zu\n", pIdxInfo->stNumKeyNodes);
	fprintf(fpOut, "  stNumLeafNodes            : %zu\n", pIdxInfo->stNumLeafNodes);
	fprintf(fpOut, "  stKeyNodeSize             : %zu\n", pIdxInfo->stKeyNodeSize);
	fprintf(fpOut, "  stLeafNodeSize            : %zu\n", pIdxInfo->stLeafNodeSize);
	fprintf(fpOut, "  stIdxDepth                : %lld\n", pIdxInfo->stIdxDepth);
	fprintf(fpOut, "  stMaxDataCnt              : %zu\n", pIdxInfo->stMaxDataCnt);
	fprintf(fpOut, "  stDataCnt                 : %zu\n", pIdxInfo->stDataCnt);

	for (size_t idx = 0; idx < pIdxInfo->stNumFlds; idx++)
	{
		fprintf(fpOut, "    Field[%zu]\n", idx);
		fprintf(fpOut, "    ----------\n");
		fprintf(fpOut, "    stDataOffset            : %zu\n", pIdxInfo->idxFlds[idx].stDataOffset);
		fprintf(fpOut, "    stLength                : %zu\n", pIdxInfo->idxFlds[idx].stLength);
		fprintf(fpOut, "    stDataType              : %zu\n", pIdxInfo->idxFlds[idx].stDataType);
	}
}

void BPPrintTreeHeader(FILE* fpOut, PBPTree pTree)
{
	fprintf(fpOut, "===========================================================================\n");
	fprintf(fpOut, "                         Tree Information\n");
	fprintf(fpOut, "===========================================================================\n");
	fprintf(fpOut, "stDataSize                              :       %08zu\n", pTree->stDataSize);
	fprintf(fpOut, "llOrder                                 :       %08lld\n", pTree->llOrder);
	BPPrintIdxInfo(fpOut, "Key", &(pTree->keyInfo));

	fprintf(fpOut, "===========================================================================\n");
}

void BPPrintNode(FILE* fpOut, PBPTree pTree, PBPNode pNode)
{
	if (pNode == NULL)
		return;

	fprintf(fpOut, "...........................................................................\n");
	fprintf(fpOut, "     Node Address                :0x%p\n", pNode);
	fprintf(fpOut, "     nodeType                    : %s\n", (BPIsLeafNode(pNode) ? "Leaf" : "Key"));
	fprintf(fpOut, "     stNodeInfo                  : 0x%016zx\n", pNode->stNodeInfo);
	fprintf(fpOut, "     stNumInNode                 : %lld\n", pNode->llNumInNode);
	fprintf(fpOut, "     pParent                     :0x%p\n", pNode->pParent);
	fprintf(fpOut, "     pLeftSibling                :0x%p\n", pNode->pLeftSibling);
	fprintf(fpOut, "     pRightSibling               :0x%p\n", pNode->pRightSibling);
	fprintf(fpOut, "     ppDataPtrArray              :0x%p\n", pNode->ppDataPtrArray);
	fprintf(fpOut, "     pChildArray                 :0x%p\n", pNode->ppChildArray);
	fprintf(fpOut, "...........................................................................\n");

	if (BPIsLeafNode(pNode) && pNode->llNumInNode == 0)
		fprintf(fpOut, "     <No Entries>\n");
	else
	{
		BPLL maxIdx = BPIsLeafNode(pNode) ? pNode->llNumInNode : (pNode->llNumInNode + 1);
		for (BPLL idx = 0; idx < maxIdx; idx++)
		{
			if (idx >= pNode->llNumInNode)
			{
				if (BPIsKeyNode(pNode))
					fprintf(fpOut, "     Idx: %05zu  Child:0x%p\n", idx, pNode->ppChildArray[idx]);
			}
			else
			{
				char* pData = pNode->ppDataPtrArray[idx];
				if (BPIsLeafNode(pNode))
				{
					fprintf(fpOut, "     Idx: %05zu                DataAddress:0x%p  Key: '", idx, pData);
					BPPrintKeyAtAddress(fpOut, pTree, pData);
					fprintf(fpOut, "'\n");
				}
				else
				{
					if(idx > pNode->llNumInNode)
						fprintf(fpOut, "     Idx: %05zu  Child:0x%p\n", idx, pNode->ppChildArray[idx]);
					else
					{
						fprintf(fpOut, "     Idx: %05zu  Child:0x%p     DataAddress:0x%p  Key: '", idx, pNode->ppChildArray[idx], pData);
						BPPrintKeyAtAddress(fpOut, pTree, pData);
						fprintf(fpOut, "'\n");
					}
				}
			}

			fflush(fpOut);
		}
	}
	fprintf(fpOut, "...........................................................................\n");
	fflush(fpOut);
}


void BPPrintNodesAtLevel(FILE* fpOut, size_t level, PBPTree pTree, PBPNode pNode)
{

	fprintf(fpOut, "***************************************************************************\n");
	fprintf(fpOut, "                      Level: %zu\n", level);
	fprintf(fpOut, "***************************************************************************\n");

	while (pNode != NULL)
	{
		BPPrintNode(fpOut, pTree, pNode);
		pNode = pNode->pRightSibling;
	}
}
void BPPrintTree(FILE* fpOut, PBPTree pTree)
{
	if (pTree == NULL)
		return;

	BPPrintTreeHeader(fpOut, pTree);

	PBPNode pNode = pTree->keyInfo.pRootNode;
	size_t level = 0;

	while (pNode != NULL)
	{
		level++;
		BPPrintNodesAtLevel(fpOut, level, pTree, pNode);

		if (BPIsLeafNode(pNode))
			pNode = NULL;
		else
			pNode = pNode->ppChildArray[0];
	}
}
