#include "BP.h"
#include <cstdint>

//extern size_t comparisonCount;
int BPKeyCmpPPRaw(size_t stNumFlds, size_t stIdxSettings, BPIdxFld idxFlds[], const void* p1, const void* p2)
{
	int result = 0;

	//	comparisonCount++;

		/* Get the address of the data elements in question */
	char* ptrData1 = (char*)p1;
	char* ptrData2 = (char*)p2;
	/* Loop through each key field doing it's datatype's compare process until one returns non-zero */
	for (size_t idx = 0; idx < stNumFlds && result == 0; idx++)
	{
		char* pFld1 = (char*)(ptrData1 + idxFlds[idx].stDataOffset);
		char* pFld2 = (char*)(ptrData2 + idxFlds[idx].stDataOffset);

		switch (idxFlds[idx].stDataType)
		{
			case BP_IDX_DATATYPE_CHAR:
				result = strncmp(pFld1, pFld2, idxFlds[idx].stLength);
				break;

			case BP_IDX_DATATYPE_BYTE:
			case BP_IDX_DATATYPE_BINARY:
				result = memcmp(pFld1, pFld2, idxFlds[idx].stLength);
				break;
				/* If we didn't have INTEL's ENDIANness ... we coulda used memcmp darn it */
			case BP_IDX_DATATYPE_SNUM_2BYTE:
			{
				short* pNum1 = (short*)pFld1;
				short* pNum2 = (short*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			case BP_IDX_DATATYPE_UNUM_2BYTE:
			{
				unsigned short* pNum1 = (unsigned short*)pFld1;
				unsigned short* pNum2 = (unsigned short*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			case BP_IDX_DATATYPE_SNUM_4BYTE:
			{
				int32_t* pNum1 = (int32_t*)pFld1;
				int32_t* pNum2 = (int32_t*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			case BP_IDX_DATATYPE_UNUM_4BYTE:
			{
				uint32_t* pNum1 = (uint32_t*)pFld1;
				uint32_t* pNum2 = (uint32_t*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			case BP_IDX_DATATYPE_SNUM_8BYTE:
			{
				BPLL* pNum1 = (BPLL*)pFld1;
				BPLL* pNum2 = (BPLL*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			case BP_IDX_DATATYPE_UNUM_8BYTE:
			{
				unsigned long long* pNum1 = (unsigned long long*)pFld1;
				unsigned long long* pNum2 = (unsigned long long*)pFld2;
				if (*pNum1 < *pNum2) result = -1;
				else if (*pNum1 > *pNum2) result = 1;
			}
			break;
			default:
				break;
		}

	}

	if (result != 0 && (stIdxSettings & BP_IDX_SETTING_SORT_DESC))
		result *= -1;

	return result;
}


int BPKeyCmpPP(PBPTree pTree, PBPIdxInfo pIdxInfo, void* p1, void* p2)
{
	return BPKeyCmpPPRaw(pIdxInfo->stNumFlds, pIdxInfo->stIdxSettings, pIdxInfo->idxFlds, p1, p2);
}
