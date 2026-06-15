#include "BP.h"

void BPIdxInfoInit(PBPIdxInfo pInfo, size_t optionalSettings)
{
	pInfo->stIdxSettings = optionalSettings;
	pInfo->pRootNode = NULL;
	pInfo->stNumFlds = 0;
	pInfo->stNumKeyNodes = 0;
	pInfo->stNumLeafNodes = 0;
	pInfo->stKeyNodeSize = 0;
	pInfo->stIdxDepth = 0;
}

BPRc BPIdxInfoAddFld(PBPIdxInfo pInfo, size_t stDataType, size_t stFldSize, size_t stFldOffset)
{
	if (pInfo->stNumFlds >= BP_IDX_MAX_KEY_FLDS)
	{
		return BP_RC_Max_Idx_Defined;
	}

	size_t newIdx = pInfo->stNumFlds;

	pInfo->idxFlds[newIdx].stDataType = stDataType;
	pInfo->idxFlds[newIdx].stDataOffset = stFldOffset;

	switch (stDataType)
	{
		case BP_IDX_DATATYPE_CHAR:
		case BP_IDX_DATATYPE_BYTE:
		case BP_IDX_DATATYPE_BINARY:
			pInfo->idxFlds[newIdx].stLength = stFldSize;
			break;
		case BP_IDX_DATATYPE_SNUM_2BYTE:
		case BP_IDX_DATATYPE_UNUM_2BYTE:
			pInfo->idxFlds[newIdx].stLength = 2;
			break;
		case BP_IDX_DATATYPE_SNUM_4BYTE:
		case BP_IDX_DATATYPE_UNUM_4BYTE:
			pInfo->idxFlds[newIdx].stLength = 4;
			break;
		case BP_IDX_DATATYPE_SNUM_8BYTE:
		case BP_IDX_DATATYPE_UNUM_8BYTE:
			pInfo->idxFlds[newIdx].stLength = 8;
			break;
		default:
			return BP_RC_Idx_Datatype_Invalid;
	}

	(pInfo->stNumFlds)++;

	return BP_RC_Success;
}
