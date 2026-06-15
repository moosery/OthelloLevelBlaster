#pragma once

#define BINSEARCH_DATASORTED_ASCENDING 'A'
#define BINSEARCH_DATASORTED_DESCENDING 'D'

#define BINSEARCH_NOT_FOUND 0xFFFFFFFFFFFFFFFF

size_t BinSearchLE(char cSortDirection, bool duplicates, bool* pEqualFound, void* pDataToSearch, size_t numEntries, size_t entrySize, size_t offsetInEntryOfDataToSendToComparisonRoutine, size_t sizeOfDataToCompare, void* pDataToFind, int (*pCmpRtn) (const void* pItem1, const void* pItem2, const size_t cmpSize));

