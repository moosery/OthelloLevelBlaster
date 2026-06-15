#include "ArenaMem.h"

PArenaMem ArenaMemCreate(size_t totalSize)
{
    PArenaMem pArena = (PArenaMem)MemMalloc("ArenaMemCreate", sizeof(ArenaMem));
    if (!pArena)
        return nullptr;

    pArena->pBase = MemMalloc("ArenaMemCreate", totalSize);
    if (!pArena->pBase)
    {
        MemFree(pArena);
        return nullptr;
    }

    pArena->totalSize            = totalSize;
    pArena->usedSize             = 0;
    pArena->pOverflowChainHead   = nullptr;
    memset(pArena->pBase, 0, totalSize);
    return pArena;
}

void ArenaMemDestroy(PArenaMem pArena)
{
    if (!pArena)
        return;
    // Free any lingering overflow nodes.
    PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.exchange(nullptr);
    while (p) { PArenaMemOverflowChainNode next = p->pNext; MemFree(p); p = next; }
    MemFree(pArena->pBase);
    MemFree(pArena);
}

void ArenaMemReset(PArenaMem pArena)
{
    if (!pArena)
        return;

    // Free overflow chain.
    PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.exchange(nullptr);
    while (p) { PArenaMemOverflowChainNode next = p->pNext; MemFree(p); p = next; }

    pArena->usedSize = 0;
}

void* ArenaMemMalloc(PArenaMem pArena, const char* pStr, size_t sizeToAlloc)
{
    if (!pArena || !pStr || sizeToAlloc == 0)
        return nullptr;

    // Align to 8 bytes so every returned pointer is naturally aligned.
    size_t aligned = (sizeToAlloc + 7) & ~7;

    size_t offset = pArena->usedSize.fetch_add(aligned);
    if (offset + aligned <= pArena->totalSize)
        return (char*)pArena->pBase + offset;

    // Overflow: arena is full.  Allocate from system heap and chain the node
    // so ArenaMemReset can free it.  No rollback of usedSize — rolling back
    // would create a race where another thread could receive the same slot.
    size_t nodeSize = sizeof(ArenaMemOverflowChainNode) - 1 + aligned;
    PArenaMemOverflowChainNode pNode = (PArenaMemOverflowChainNode)MemMalloc(pStr, nodeSize);
    if (!pNode)
        return nullptr;

    do
    {
        pNode->pNext = pArena->pOverflowChainHead.load();
    } while (!pArena->pOverflowChainHead.compare_exchange_weak(pNode->pNext, pNode));

    return pNode->data;
}

size_t ArenaMemSize(PArenaMem pArena)
{
    if (!pArena)
        return 0;
    return pArena->usedSize.load();
}

bool ArenaMemHasOverflowed(PArenaMem pArena)
{
    if (!pArena)
        return false;
    return pArena->pOverflowChainHead.load(std::memory_order_relaxed) != nullptr;
}

void ArenaMemStatsPrint(PArenaMem pArena, FILE* fpOut)
{
    if (!pArena || !fpOut)
        return;

    size_t used     = pArena->usedSize.load();
    size_t capped   = (used < pArena->totalSize) ? used : pArena->totalSize;
    int    overflow = 0;
    for (PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.load(); p; p = p->pNext)
        overflow++;

    fprintf(fpOut, "ArenaMem Stats:\n");
    fprintf(fpOut, "  Total Size:      %zu bytes\n", pArena->totalSize);
    fprintf(fpOut, "  Used Size:       %zu bytes (%.1f%%)\n",
            capped, pArena->totalSize ? 100.0 * capped / pArena->totalSize : 0.0);
    fprintf(fpOut, "  Overflow Nodes:  %d\n", overflow);
}
