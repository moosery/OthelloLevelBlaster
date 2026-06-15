#pragma once
#include <memory.h>
#include <stdio.h>
void* MemMalloc(const char* pStr, size_t sizeToAlloc);
void MemFree(void* pPtr);
size_t MemSize();
void MemStatsPrint(FILE *fpOut);
void MemCheck(FILE *fpOut, const char *pszStr);


