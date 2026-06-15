#pragma once
#include <memory.h>
#include <stdio.h>
#include "Mem.h"
#include <atomic>

typedef struct ArenaMemOverflowChainNode
{
    struct ArenaMemOverflowChainNode* pNext;
    char data[1]; // variable-length data starts here; alloc = sizeof(node)-1+size
} ArenaMemOverflowChainNode, * PArenaMemOverflowChainNode;

typedef struct ArenaMem
{
    void*               pBase;
    size_t              totalSize;
    std::atomic_size_t  usedSize;
    std::atomic<PArenaMemOverflowChainNode> pOverflowChainHead;
} ArenaMem, * PArenaMem;

PArenaMem ArenaMemCreate(size_t totalSize);
void ArenaMemDestroy(PArenaMem pArena);
void ArenaMemReset(PArenaMem pArena);
void* ArenaMemMalloc(PArenaMem pArena, const char* pStr, size_t sizeToAlloc);
#define ArenaMemFree(pArena, pPtr) ((void)0)
size_t ArenaMemSize(PArenaMem pArena);
bool ArenaMemHasOverflowed(PArenaMem pArena);
void ArenaMemStatsPrint(PArenaMem pArena, FILE* fpOut);
#define ArenaMemCheck(pArena, fpOut, pszStr) ((void)0)
