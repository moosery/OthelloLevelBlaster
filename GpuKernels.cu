#include "GpuKernels.h"
#include "OthelloBasics.h"           // BOARD_KEY (used internally in ExpandKernel)
#include "OthelloBasicsForCUDA.h"
#include "Error.h"
#include <cuda_runtime.h>
#include <cub/cub.cuh>
#include <string.h>

// ============================================================================
// CUDA error helper
// ============================================================================

#define GPU_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) \
            Fatal(FATAL_GPU_ERROR, "CUDA error %s:%d  %s", \
                  __FILE__, __LINE__, cudaGetErrorString(_e)); \
    } while (0)

// ============================================================================
// GpuAccumulator memory layout (two-stack BOARD_KEY_DISK edition)
//
// Per-batch (device):
//   d_input          [batchSize] BOARD_KEY_DISK    H2D staging
//   d_blackWritePos  uint32_t                      atomic counter for black stack
//   d_whiteWritePos  uint32_t                      atomic counter for white stack
//
// Per-batch (pinned host):
//   h_input          [batchSize] BOARD_KEY_DISK
//   h_blackWritePos  uint32_t
//   h_whiteWritePos  uint32_t
//
// Large buffers (device) — sized by accumCapacity:
//   d_accum   [accumCapacity] BOARD_KEY_DISK  two-stack layout:
//               black stack: indices [0 .. blackWriteOffset-1]  (grows up)
//               white stack: indices [cap-1 .. cap-whiteWriteOffset] (grows down)
//   d_gather  [accumCapacity] BOARD_KEY_DISK  sorted+deduped output:
//               [0 .. blackUnique-1]            black boards
//               [blackUnique .. blackUnique+whiteUnique-1]  white boards
//   d_fieldA  [accumCapacity] uint64_t   sort key ping-pong A / not-flags temp
//   d_fieldB  [accumCapacity] uint64_t   sort key ping-pong B
//   d_indicesA[accumCapacity] uint32_t   sort value ping-pong A (final permutation)
//   d_indicesB[accumCapacity] uint32_t   sort value ping-pong B / prefix-sum output
//   d_flags   [accumCapacity] uint8_t    dup flags (1 = duplicate)
//
// Memory budget: 80% of totalGpuBytes.
//   Expand overhead: batchSize*16 + 8 bytes (d_input + two atomic counters).
//   Per accumCapacity slot: 16+16+8+8+4+4+1 = 57 bytes.
//
// vs. old BOARD_KEY design: 73 bytes/slot + batchSize*(1+maxMoves)*24 expand overhead.
// Result: ~50% more accumulator capacity on same GPU for typical 6x6 parameters.
// ============================================================================

struct __GpuAccumulator
{
    // Per-batch (device)
    BOARD_KEY_DISK* d_input;
    uint32_t*       d_blackWritePos;
    uint32_t*       d_whiteWritePos;

    // Per-batch (pinned host)
    BOARD_KEY_DISK* h_input;
    uint32_t*       h_blackWritePos;
    uint32_t*       h_whiteWritePos;

    // Large device buffers
    BOARD_KEY_DISK* d_accum;
    BOARD_KEY_DISK* d_gather;
    uint64_t*       d_fieldA;
    uint64_t*       d_fieldB;
    uint32_t*       d_indicesA;
    uint32_t*       d_indicesB;
    uint8_t*        d_flags;
    uint32_t*       d_batchStats;    // [0]=pass, [1]=terminal, [2]=maxMoves
    uint32_t        h_batchStats[3];

    // Pre-allocated CUB temp storage
    void*  d_sortTemp;
    size_t sortTempBytes;
    void*  d_scanTemp;
    size_t scanTempBytes;

    cudaStream_t   stream;
    DevBoardConsts boardConsts;

    int    batchSize;
    int    maxMovesPerBoard;
    int    numRotations;
    size_t accumCapacity;

    size_t blackWriteOffset;   // total black boards written since last reset
    size_t whiteWriteOffset;   // total white boards written since last reset
    int    uniqueBlackCount;   // set by GpuFlushPrepare
    int    uniqueWhiteCount;   // set by GpuFlushPrepare
    int    uniqueCount;        // = uniqueBlackCount + uniqueWhiteCount
};

// ============================================================================
// Expansion kernel
//
// One thread per input board.  Reconstructs a full BOARD_KEY from the
// BOARD_KEY_DISK input + the batch-wide playerBit.  Computes child boards and
// scatters them directly into the two-stack d_accum via atomic counters:
//   black children -> d_accum[pos]          where pos = atomicAdd(d_blackWritePos)
//   white children -> d_accum[cap-1-pos]    where pos = atomicAdd(d_whiteWritePos)
//
// No separate ScatterKernel is needed; the intermediate results array is
// eliminated entirely, saving ~20x batchSize * sizeof(BOARD_KEY) device memory.
// ============================================================================

__global__ void ExpandKernel(
    const BOARD_KEY_DISK* __restrict__ input,
    int                                batchSize,
    uint8_t                            inputPlayerBit,
    DevBoardConsts                     consts,
    int                                numRotations,
    uint32_t*                          d_batchStats,
    BOARD_KEY_DISK*                    d_accum,
    uint32_t                           accumCapacity,
    uint32_t*                          d_blackWritePos,
    uint32_t*                          d_whiteWritePos)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= batchSize) return;

    // Reconstruct BOARD_KEY from 16-byte disk record + batch player bit
    const BOARD_KEY_DISK srcDisk = input[i];
    BOARD_KEY src = {};
    src.ullCellsInUse = srcDisk.ullCellsInUse;
    src.ullCellColors = srcDisk.ullCellColors;
    src.usBoardInfo   = inputPlayerBit;

    unsigned long long moves = dev_boardKeyGetMoves(&src, consts);

    if (moves == 0)
    {
        // Current player can't move — try pass
        BOARD_KEY passKey = src;
        if ((passKey.usBoardInfo & 0x01u) == 0u)
            passKey.usBoardInfo |=  0x01u;
        else
            passKey.usBoardInfo &= ~0x01u;

        unsigned long long oppMoves = dev_boardKeyGetMoves(&passKey, consts);
        if (oppMoves == 0)
        {
            atomicAdd(&d_batchStats[1], 1u);  // terminal: both players stuck
            return;
        }

        atomicAdd(&d_batchStats[0], 1u);  // pass: opponent moves
        moves = oppMoves;
        src   = passKey;  // expand opponent's moves as next-level children
    }

    int count = 0;
    while (moves)
    {
        int moveIdx = __clzll(moves);
        moves &= ~(0x8000000000000000ULL >> moveIdx);

        BOARD_KEY child = {};
        dev_playMove_key(&src, &child, moveIdx);
        dev_canonicalize_key(&child, numRotations);

        BOARD_KEY_DISK childDisk = { child.ullCellsInUse, child.ullCellColors };
        int childPlayer = child.usBoardInfo & 0x01;

        if (childPlayer == 1)  // black
        {
            uint32_t pos = atomicAdd(d_blackWritePos, 1u);
            if (pos < accumCapacity)
                d_accum[pos] = childDisk;
        }
        else  // white
        {
            uint32_t pos = atomicAdd(d_whiteWritePos, 1u);
            if (pos < accumCapacity)
                d_accum[accumCapacity - 1u - pos] = childDisk;
        }
        count++;
    }

    atomicMax(&d_batchStats[2], (uint32_t)count);
}

// ============================================================================
// Sort helpers — 2-pass CUB DeviceRadixSort::SortPairs on BOARD_KEY_DISK fields.
// LSB-first: f1 (ullCellColors) then f0 (ullCellsInUse) → ascending by (inUse, colors).
// Each kernel takes a base pointer so it can operate on either the black or white region.
// ============================================================================

__global__ void InitIndicesKernel(uint32_t* indices, uint32_t count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) indices[i] = i;
}

__global__ void ExtractFieldKernel(
    const BOARD_KEY_DISK* __restrict__ boards,
    uint32_t                           count,
    int                                fieldIdx,  // 0=ullCellsInUse  1=ullCellColors
    uint64_t*                          out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    out[i] = (fieldIdx == 0) ? boards[i].ullCellsInUse : boards[i].ullCellColors;
}

__global__ void GatherFieldKernel(
    const BOARD_KEY_DISK* __restrict__ boards,
    const uint32_t*       __restrict__ perm,
    uint32_t                           count,
    int                                fieldIdx,
    uint64_t*                          out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const BOARD_KEY_DISK& b = boards[perm[i]];
    out[i] = (fieldIdx == 0) ? b.ullCellsInUse : b.ullCellColors;
}

// ============================================================================
// Dedup kernels
// ============================================================================

// flags[i] = 1 if boards[perm[i]] equals boards[perm[i-1]] (a duplicate).
__global__ void MarkDupFlagsKernel(
    const BOARD_KEY_DISK* __restrict__ boards,
    const uint32_t*       __restrict__ perm,
    uint8_t*                           flags,
    uint32_t                           count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    if (i == 0) { flags[0] = 0; return; }
    const BOARD_KEY_DISK& a = boards[perm[i - 1]];
    const BOARD_KEY_DISK& b = boards[perm[i]];
    flags[i] = (a.ullCellsInUse == b.ullCellsInUse && a.ullCellColors == b.ullCellColors) ? 1u : 0u;
}

// notFlags[i] = 1 if flags[i] == 0.  Written as uint32_t for cub::DeviceScan.
__global__ void InvertFlagsKernel(
    const uint8_t* __restrict__ flags,
    uint32_t*                   notFlags,
    uint32_t                    count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) notFlags[i] = flags[i] ? 0u : 1u;
}

// Scatter unique boards (in sorted order) into d_gather at gatherOffset + outPos[i].
__global__ void CompactKernel(
    const BOARD_KEY_DISK* __restrict__ accum,
    const uint32_t*       __restrict__ perm,
    const uint8_t*        __restrict__ flags,
    const uint32_t*       __restrict__ outPos,
    uint32_t                           count,
    BOARD_KEY_DISK*                    out,
    uint32_t                           gatherOffset)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count || flags[i]) return;
    out[gatherOffset + outPos[i]] = accum[perm[i]];
}

// ============================================================================
// API implementation
// ============================================================================

GpuAccumulator* GpuAccumulatorCreate(int batchSize, int maxMovesPerBoard, size_t totalGpuBytes)
{
    GpuAccumulator* p = new GpuAccumulator();
    memset(p, 0, sizeof(*p));

    p->batchSize        = batchSize;
    p->maxMovesPerBoard = maxMovesPerBoard;
    p->numRotations     = 16;
    p->boardConsts      = OBCuda_GetBoardConsts();

    // Per-slot device cost: 16(d_accum)+16(d_gather)+8(fieldA)+8(fieldB)+4(indA)+4(indB)+1(flags)=57
    // Expand overhead: batchSize*16 (d_input) + 2*4 (two atomic counters)
    size_t expandBytes = (size_t)batchSize * sizeof(BOARD_KEY_DISK) + 2 * sizeof(uint32_t);
    size_t budget      = totalGpuBytes * 8 / 10;
    if (budget > expandBytes) budget -= expandBytes;
    p->accumCapacity   = budget / 57;

    GPU_CHECK(cudaStreamCreate(&p->stream));

    // Per-batch device allocations
    GPU_CHECK(cudaMalloc(&p->d_input,         (size_t)batchSize * sizeof(BOARD_KEY_DISK)));
    GPU_CHECK(cudaMalloc(&p->d_blackWritePos, sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_whiteWritePos, sizeof(uint32_t)));

    // Pinned host buffers
    GPU_CHECK(cudaMallocHost(&p->h_input,         (size_t)batchSize * sizeof(BOARD_KEY_DISK)));
    GPU_CHECK(cudaMallocHost(&p->h_blackWritePos, sizeof(uint32_t)));
    GPU_CHECK(cudaMallocHost(&p->h_whiteWritePos, sizeof(uint32_t)));

    // Large device buffers
    GPU_CHECK(cudaMalloc(&p->d_accum,    p->accumCapacity * sizeof(BOARD_KEY_DISK)));
    GPU_CHECK(cudaMalloc(&p->d_gather,   p->accumCapacity * sizeof(BOARD_KEY_DISK)));
    GPU_CHECK(cudaMalloc(&p->d_fieldA,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_fieldB,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesA, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesB, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_flags,      p->accumCapacity * sizeof(uint8_t)));
    GPU_CHECK(cudaMalloc(&p->d_batchStats, 3 * sizeof(uint32_t)));
    GPU_CHECK(cudaMemset( p->d_batchStats, 0, 3 * sizeof(uint32_t)));

    // Pre-allocate CUB temp storage for sort and prefix-sum (sized for worst-case = accumCapacity)
    {
        cub::DoubleBuffer<uint64_t> kq(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vq(p->d_indicesA, p->d_indicesB);
        p->sortTempBytes = 0;
        cub::DeviceRadixSort::SortPairs(nullptr, p->sortTempBytes, kq, vq, (int)p->accumCapacity);
        GPU_CHECK(cudaMalloc(&p->d_sortTemp, p->sortTempBytes));
    }
    {
        uint32_t* dummy = reinterpret_cast<uint32_t*>(p->d_fieldA);
        p->scanTempBytes = 0;
        cub::DeviceScan::ExclusiveSum(nullptr, p->scanTempBytes, dummy, p->d_indicesB, (int)p->accumCapacity);
        GPU_CHECK(cudaMalloc(&p->d_scanTemp, p->scanTempBytes));
    }

    GPU_CHECK(cudaMemset(p->d_blackWritePos, 0, sizeof(uint32_t)));
    GPU_CHECK(cudaMemset(p->d_whiteWritePos, 0, sizeof(uint32_t)));
    return p;
}

void GpuAccumulatorDestroy(GpuAccumulator* pAccum)
{
    cudaStreamSynchronize(pAccum->stream);
    cudaStreamDestroy(pAccum->stream);
    cudaFree(pAccum->d_input);
    cudaFree(pAccum->d_blackWritePos);
    cudaFree(pAccum->d_whiteWritePos);
    cudaFreeHost(pAccum->h_input);
    cudaFreeHost(pAccum->h_blackWritePos);
    cudaFreeHost(pAccum->h_whiteWritePos);
    cudaFree(pAccum->d_accum);
    cudaFree(pAccum->d_gather);
    cudaFree(pAccum->d_fieldA);
    cudaFree(pAccum->d_fieldB);
    cudaFree(pAccum->d_indicesA);
    cudaFree(pAccum->d_indicesB);
    cudaFree(pAccum->d_flags);
    cudaFree(pAccum->d_batchStats);
    cudaFree(pAccum->d_sortTemp);
    cudaFree(pAccum->d_scanTemp);
    delete pAccum;
}

bool GpuAccumulatorHasRoom(const GpuAccumulator* pAccum, int nextBatchCount)
{
    size_t worstCase = (size_t)nextBatchCount * (size_t)pAccum->maxMovesPerBoard;
    size_t used      = pAccum->blackWriteOffset + pAccum->whiteWriteOffset;
    return (used + worstCase) <= pAccum->accumCapacity;
}

void GpuProcessBatch(GpuAccumulator* pAccum, const BOARD_KEY_DISK* pBoards,
                     int count, uint8_t playerBit)
{
    cudaStream_t s = pAccum->stream;

    memcpy(pAccum->h_input, pBoards, (size_t)count * sizeof(BOARD_KEY_DISK));
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_input, pAccum->h_input,
                              (size_t)count * sizeof(BOARD_KEY_DISK),
                              cudaMemcpyHostToDevice, s));

    // Set atomic counters to current accumulated offsets before expand
    uint32_t bwo = (uint32_t)pAccum->blackWriteOffset;
    uint32_t wwo = (uint32_t)pAccum->whiteWriteOffset;
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_blackWritePos, &bwo, sizeof(uint32_t),
                              cudaMemcpyHostToDevice, s));
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_whiteWritePos, &wwo, sizeof(uint32_t),
                              cudaMemcpyHostToDevice, s));

    // Expand + direct two-stack scatter (one thread per input board)
    {
        int threads = 256;
        int blocks  = (count + threads - 1) / threads;
        ExpandKernel<<<blocks, threads, 0, s>>>(
            pAccum->d_input, count, playerBit,
            pAccum->boardConsts, pAccum->numRotations,
            pAccum->d_batchStats,
            pAccum->d_accum, (uint32_t)pAccum->accumCapacity,
            pAccum->d_blackWritePos, pAccum->d_whiteWritePos);
    }

    // D2H updated counters to track stack sizes on the host
    GPU_CHECK(cudaMemcpyAsync(pAccum->h_blackWritePos, pAccum->d_blackWritePos,
                              sizeof(uint32_t), cudaMemcpyDeviceToHost, s));
    GPU_CHECK(cudaMemcpyAsync(pAccum->h_whiteWritePos, pAccum->d_whiteWritePos,
                              sizeof(uint32_t), cudaMemcpyDeviceToHost, s));
    GPU_CHECK(cudaStreamSynchronize(s));

    pAccum->blackWriteOffset = *pAccum->h_blackWritePos;
    pAccum->whiteWriteOffset = *pAccum->h_whiteWritePos;
}

// ============================================================================
// SortAndDedupRegion — internal helper called by GpuFlushPrepare.
//
// Sorts region [base .. base+N-1] by (ullCellsInUse, ullCellColors),
// deduplicates, and compacts unique boards into out[gatherOffset..].
// Returns the unique count.
// ============================================================================

static int SortAndDedupRegion(GpuAccumulator* p,
                               BOARD_KEY_DISK* base, uint32_t N,
                               BOARD_KEY_DISK* out, uint32_t gatherOffset)
{
    if (N == 0) return 0;

    int threads = 256;
    int blocks  = ((int)N + threads - 1) / threads;

    void*  d_temp    = p->d_sortTemp;
    size_t tempBytes = p->sortTempBytes;

    // Pass 1: sort by f1 (ullCellColors — LSB field)
    ExtractFieldKernel<<<blocks, threads>>>(base, N, 1, p->d_fieldA);
    InitIndicesKernel <<<blocks, threads>>>(p->d_indicesA, N);
    {
        cub::DoubleBuffer<uint64_t> kDb(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(p->d_indicesA, p->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(p->d_indicesA, p->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // Pass 2: sort by f0 (ullCellsInUse — MSB field)
    GatherFieldKernel<<<blocks, threads>>>(base, p->d_indicesA, N, 0, p->d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> kDb(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(p->d_indicesA, p->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(p->d_indicesA, p->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // Mark adjacent duplicates in sorted order
    MarkDupFlagsKernel<<<blocks, threads>>>(base, p->d_indicesA, p->d_flags, N);

    // Compact: invert flags, prefix-sum for output positions, scatter unique boards
    uint32_t* d_notFlags = reinterpret_cast<uint32_t*>(p->d_fieldA);
    InvertFlagsKernel<<<blocks, threads>>>(p->d_flags, d_notFlags, N);

    size_t scanBytes = p->scanTempBytes;
    GPU_CHECK((cub::DeviceScan::ExclusiveSum(p->d_scanTemp, scanBytes,
                                              d_notFlags, p->d_indicesB, (int)N)));

    CompactKernel<<<blocks, threads>>>(base, p->d_indicesA, p->d_flags,
                                       p->d_indicesB, N, out, gatherOffset);
    GPU_CHECK(cudaDeviceSynchronize());

    // uniqueCount = last prefix-sum value + last not-flag value
    uint32_t h_lastPos, h_lastNotFlag;
    GPU_CHECK(cudaMemcpy(&h_lastPos,     p->d_indicesB + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
    GPU_CHECK(cudaMemcpy(&h_lastNotFlag, d_notFlags    + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
    return (int)(h_lastPos + h_lastNotFlag);
}

// ============================================================================
// GpuFlushPrepare
//
// Runs sort+dedup independently on the black region [0..B-1] and the white
// region [cap-W..cap-1].  Output in d_gather: black first, then white.
// ============================================================================

int GpuFlushPrepare(GpuAccumulator* pAccum)
{
    uint32_t B = (uint32_t)pAccum->blackWriteOffset;
    if (B > (uint32_t)pAccum->accumCapacity) B = (uint32_t)pAccum->accumCapacity;

    uint32_t wCap = (uint32_t)pAccum->accumCapacity - B;
    uint32_t W    = (uint32_t)pAccum->whiteWriteOffset;
    if (W > wCap) W = wCap;

    if (B == 0 && W == 0)
    {
        GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                             3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
        pAccum->uniqueBlackCount = 0;
        pAccum->uniqueWhiteCount = 0;
        pAccum->uniqueCount      = 0;
        return 0;
    }

    // Black region: d_accum[0..B-1]
    pAccum->uniqueBlackCount = SortAndDedupRegion(
        pAccum, pAccum->d_accum, B, pAccum->d_gather, 0);

    // White region: d_accum[cap-W..cap-1]  (stored reverse-chronologically; sort fixes order)
    BOARD_KEY_DISK* whiteBase = pAccum->d_accum + pAccum->accumCapacity - W;
    pAccum->uniqueWhiteCount = SortAndDedupRegion(
        pAccum, whiteBase, W, pAccum->d_gather, (uint32_t)pAccum->uniqueBlackCount);

    pAccum->uniqueCount = pAccum->uniqueBlackCount + pAccum->uniqueWhiteCount;

    GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                         3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    return pAccum->uniqueCount;
}

// ============================================================================
// GpuFlushRead
// ============================================================================

int GpuFlushRead(GpuAccumulator* pAccum, int player, size_t offset,
                 BOARD_KEY_DISK* pOut, int maxCount)
{
    int total      = (player == 1) ? pAccum->uniqueBlackCount : pAccum->uniqueWhiteCount;
    int baseOffset = (player == 1) ? 0                        : pAccum->uniqueBlackCount;
    int avail = total - (int)offset;
    if (avail <= 0) return 0;
    int got = (avail < maxCount) ? avail : maxCount;
    GPU_CHECK(cudaMemcpy(pOut, pAccum->d_gather + baseOffset + (int)offset,
                         (size_t)got * sizeof(BOARD_KEY_DISK), cudaMemcpyDeviceToHost));
    return got;
}

// ============================================================================
// GpuFlushReset
// ============================================================================

void GpuFlushReset(GpuAccumulator* pAccum)
{
    GPU_CHECK(cudaMemsetAsync(pAccum->d_blackWritePos, 0, sizeof(uint32_t), pAccum->stream));
    GPU_CHECK(cudaMemsetAsync(pAccum->d_whiteWritePos, 0, sizeof(uint32_t), pAccum->stream));
    GPU_CHECK(cudaMemsetAsync(pAccum->d_batchStats,    0, 3 * sizeof(uint32_t), pAccum->stream));
    pAccum->blackWriteOffset = 0;
    pAccum->whiteWriteOffset = 0;
    pAccum->uniqueBlackCount = 0;
    pAccum->uniqueWhiteCount = 0;
    pAccum->uniqueCount      = 0;
    pAccum->h_batchStats[0]  = 0;
    pAccum->h_batchStats[1]  = 0;
    pAccum->h_batchStats[2]  = 0;
}

// ============================================================================
// Accessors
// ============================================================================

size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum)
{
    return pAccum->blackWriteOffset + pAccum->whiteWriteOffset;
}

int GpuFlushBlackCount(const GpuAccumulator* pAccum) { return pAccum->uniqueBlackCount; }
int GpuFlushWhiteCount(const GpuAccumulator* pAccum) { return pAccum->uniqueWhiteCount; }

uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum) { return pAccum->h_batchStats[0]; }
uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum) { return pAccum->h_batchStats[1]; }
uint32_t GpuFlushMaxMoves  (const GpuAccumulator* pAccum) { return pAccum->h_batchStats[2]; }
