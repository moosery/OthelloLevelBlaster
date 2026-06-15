#include "BinSearchLE.h"

size_t BinSearchLE(char cSortDirection, bool duplicates, bool* pEqualFound, void* pDataToSearch, size_t numEntries, size_t entrySize, size_t offsetInEntryOfDataToSendToComparisonRoutine, size_t sizeOfDataToCompare, void* pDataToFind, int (*pCmpRtn) (const void* pItem1, const void* pItem2, const size_t cmpSize))
{
    char* pDataToSearchAsChar = (char*)pDataToSearch;
    char* pDataToFindAsChar = (char*)pDataToFind;
    size_t low = 0;
    size_t high = numEntries;
    size_t result = BINSEARCH_NOT_FOUND;
    int direction = (cSortDirection == BINSEARCH_DATASORTED_ASCENDING ? 1 : -1);
    *pEqualFound = false;

    if (numEntries == 0)
        return result;

    if (direction == 1)
    {
        while (low < high)
        {
            size_t mid = low + ((high - low) >> 1);
            int cmpResult = pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + mid * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare);
            if (cmpResult >= 0)
                low = mid + 1;
            else
                high = mid;
        }
        result = (low > 0) ? low - 1 : BINSEARCH_NOT_FOUND;
    }
    else
    {
        while (low < high)
        {
            size_t mid = low + ((high - low) >> 1);
            int cmpResult = pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + mid * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare);
            if (cmpResult < 0)
                low = mid + 1;
            else
                high = mid;
        }
        result = (low < numEntries) ? low : BINSEARCH_NOT_FOUND;
    }

    if (result != BINSEARCH_NOT_FOUND)
    {
        if (pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + result * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare) == 0)
            *pEqualFound = true;
    }

    /* Back up over duplicates if there */
    if (result != BINSEARCH_NOT_FOUND && duplicates && !(direction == 1 && result == 0))
    {
        char* rowOrigVal = pDataToSearchAsChar + result * entrySize + offsetInEntryOfDataToSendToComparisonRoutine;
        size_t pos = result - direction;
        char* pRec = pDataToSearchAsChar + pos * entrySize;
        while (pRec >= pDataToSearchAsChar && pos < numEntries)
        {
            if (pCmpRtn(rowOrigVal, pRec + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare) != 0)
                break;
            result = pos;
            pos -= direction;
            pRec = pDataToSearchAsChar + pos * entrySize;
        }
    }

    return result;
}
