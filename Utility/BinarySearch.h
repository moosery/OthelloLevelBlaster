#pragma once
#include <stdio.h>

long long BinarySearch(void* dataArray, void* pKey, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext);
long long BinarySearchFile(FILE* fpOut, void* pKey, void* pDataBuffer, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext);