#include "GpuKernels.h"
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
// GpuAccumulator memory layout
//
// Per-batch (device) — sized by batchSize:
//   d_input      [batchSize] BOARD_KEY     H2D staging
//   d_results    [batchSize x maxMoves] BOARD_KEY  per-board slot array
//   d_counts     [batchSize] int            valid moves per input board
//   d_writePos   uint32_t                   atomic scatter counter
//
// Per-batch (pinned host):
//   h_input      [batchSize] BOARD_KEY
//   h_writePos   uint32_t
//
// Large accum/sort/dedup/compact (device) — sized by accumCapacity:
//   d_accum      [accumCapacity] BOARD_KEY  raw expansion output
//   d_gather     [accumCapacity] BOARD_KEY  compacted unique output (sorted)
//   d_fieldA     [accumCapacity] uint64_t   sort key ping-pong A / not-flags temp
//   d_fieldB     [accumCapacity] uint64_t   sort key ping-pong B
//   d_indicesA   [accumCapacity] uint32_t   sort value ping-pong A (final permutation)
//   d_indicesB   [accumCapacity] uint32_t   sort value ping-pong B / prefix-sum output
//   d_flags      [accumCapacity] uint8_t    dup flags (1 = duplicate)
//
// Memory budget: 80% of totalGpuBytes divided by 73 bytes/slot
// (24 accum + 24 gather + 16 fields + 8 indices + 1 flag = 73)
// gives accumCapacity ~175 M on a 16 GB card.
// ============================================================================

struct __GpuAccumulator
{
    // Per-batch (device)
    BOARD_KEY*  d_input;
    BOARD_KEY*  d_results;
    int*        d_counts;
    uint32_t*   d_writePos;

    // Per-batch (pinned host)
    BOARD_KEY*  h_input;
    uint32_t*   h_writePos;

    // Large buffers (device)
    BOARD_KEY*  d_accum;
    BOARD_KEY*  d_gather;
    uint64_t*   d_fieldA;
    uint64_t*   d_fieldB;
    uint32_t*   d_indicesA;
    uint32_t*   d_indicesB;
    uint8_t*    d_flags;
    uint32_t*   d_batchStats;     // [0]=passBoards, [1]=terminalBoards, [2]=maxMoves (device atomics)
    uint32_t    h_batchStats[3];  // host copy, populated by GpuFlushPrepare

    // Pre-allocated CUB temp storage (reused every flush)
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
    size_t writeOffset;
    int    uniqueCount;     // set by GpuFlushPrepare; used by GpuFlushRead
};

// ============================================================================
// Expansion kernel — one thread per input board.
//
// Writes up to maxMovesPerBoard children into the fixed per-board slot array.
//
// Pass handling: when the current player has no moves, the opponent's moves
// are expanded immediately and their children emitted as level-N+1 output.
// A pass board itself is never emitted — it doesn't advance the piece count
// and must not occupy a level-file slot.
// ============================================================================

__global__ void ExpandKernel(
    const BOARD_KEY* __restrict__ input,
    BOARD_KEY*                    results,
    int*                          counts,
    int                           batchSize,
    int                           maxMovesPerBoard,
    DevBoardConsts                consts,
    int                           numRotations,
    uint32_t*                     d_batchStats)   // [0]=pass, [1]=terminal
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= batchSize) return;

    const BOARD_KEY src     = input[i];
    BOARD_KEY*      mySlots = results + (size_t)i * (size_t)maxMovesPerBoard;
    int             count   = 0;

    unsigned long long moves = dev_boardKeyGetMoves(&src, consts);

    if (moves == 0)
    {
        // Current player can't move — try the other player (pass)
        BOARD_KEY passKey = src;
        if ((passKey.usBoardInfo & 0x01u) == 0u)
            passKey.usBoardInfo |=  0x01u;
        else
            passKey.usBoardInfo &= ~0x01u;

        unsigned long long oppMoves = dev_boardKeyGetMoves(&passKey, consts);
        if (oppMoves == 0)
        {
            atomicAdd(&d_batchStats[1], 1u);   // terminal: both players stuck
            counts[i] = 0;
            return;
        }

        atomicAdd(&d_batchStats[0], 1u);   // pass: current player skips, opponent moves

        // Expand opponent's moves directly — these are the true level-N+1 children
        moves = oppMoves;
        while (moves)
        {
            int moveIdx = __clzll(moves);
            moves &= ~(0x8000000000000000ULL >> moveIdx);
            BOARD_KEY child = {};
            dev_playMove_key(&passKey, &child, moveIdx);
            dev_canonicalize_key(&child, numRotations);
            if (count < maxMovesPerBoard) mySlots[count] = child;
            count++;
        }
    }
    else
    {
        while (moves)
        {
            int moveIdx = __clzll(moves);
            moves &= ~(0x8000000000000000ULL >> moveIdx);
            BOARD_KEY child = {};
            dev_playMove_key(&src, &child, moveIdx);
            dev_canonicalize_key(&child, numRotations);
            if (count < maxMovesPerBoard) mySlots[count] = child;
            count++;
        }
    }

    counts[i] = count;
    atomicMax(&d_batchStats[2], (uint32_t)count);
}

// ============================================================================
// Scatter kernel — compacts valid per-board slots into the accumulation buffer.
// One thread per (board x slot) pair; slots beyond counts[board] are skipped.
// ============================================================================

__global__ void ScatterKernel(
    const BOARD_KEY* __restrict__ results,
    const int*       __restrict__ counts,
    int                           batchSize,
    int                           maxMovesPerBoard,
    BOARD_KEY*                    accum,
    uint32_t                      accumCapacity,
    uint32_t*                     d_writePos)
{
    int tid   = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int total = batchSize * maxMovesPerBoard;
    if (tid >= total) return;

    int board = tid / maxMovesPerBoard;
    int slot  = tid % maxMovesPerBoard;
    if (slot >= counts[board]) return;

    uint32_t pos = atomicAdd(d_writePos, 1u);
    if (pos < accumCapacity)
        accum[pos] = results[tid];
}

// ============================================================================
// Sort helpers — 3-pass CUB DeviceRadixSort::SortPairs on uint64_t fields.
//
// Three stable radix sort passes (LSB-first: f2 -> f1 -> f0) produce a
// permutation in d_indicesA that gives ascending order by
// (ullCellsInUse, ullCellColors, usBoardInfo+pad).
// ============================================================================

__global__ void InitIndicesKernel(uint32_t* indices, uint32_t count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) indices[i] = i;
}

__global__ void ExtractFieldKernel(
    const BOARD_KEY* __restrict__ boards,
    uint32_t                      count,
    int                           fieldIdx,
    uint64_t*                     out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    out[i] = reinterpret_cast<const uint64_t*>(&boards[i])[fieldIdx];
}

__global__ void GatherFieldKernel(
    const BOARD_KEY* __restrict__ boards,
    const uint32_t*  __restrict__ perm,
    uint32_t                      count,
    int                           fieldIdx,
    uint64_t*                     out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    out[i] = reinterpret_cast<const uint64_t*>(&boards[perm[i]])[fieldIdx];
}

// ============================================================================
// Dedup kernels
// ============================================================================

// flags[i] = 1 if boards[perm[i]] equals boards[perm[i-1]] (a duplicate).
__global__ void MarkDupFlagsKernel(
    const BOARD_KEY* __restrict__ boards,
    const uint32_t*  __restrict__ perm,
    uint8_t*                      flags,
    uint32_t                      count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    if (i == 0) { flags[0] = 0; return; }
    const uint64_t* a = reinterpret_cast<const uint64_t*>(&boards[perm[i - 1]]);
    const uint64_t* b = reinterpret_cast<const uint64_t*>(&boards[perm[i]]);
    flags[i] = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 1u : 0u;
}

// notFlags[i] = 1 if flags[i] == 0 (unique), 0 otherwise.
// Written as uint32_t so cub::DeviceScan::ExclusiveSum can consume it directly.
// d_fieldA (uint64_t array) is repurposed here — each uint64_t slot stores one
// uint32_t in its lower 32 bits (the upper 32 bits are don't-care).
__global__ void InvertFlagsKernel(
    const uint8_t* __restrict__ flags,
    uint32_t*                   notFlags,
    uint32_t                    count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) notFlags[i] = flags[i] ? 0u : 1u;
}

// Scatter unique boards in sorted order into the contiguous d_gather buffer.
// outPos[i] is the exclusive prefix-sum of notFlags; for non-duplicate entries
// it gives the destination slot in d_gather for d_accum[perm[i]].
__global__ void CompactKernel(
    const BOARD_KEY* __restrict__ accum,
    const uint32_t*  __restrict__ perm,
    const uint8_t*   __restrict__ flags,
    const uint32_t*  __restrict__ outPos,
    uint32_t                      count,
    BOARD_KEY*                    out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count || flags[i]) return;
    out[outPos[i]] = accum[perm[i]];
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

    // Per-batch expand buffers are a fixed overhead; subtract them first.
    // Each accumCapacity slot needs:
    //   24 (d_accum) + 24 (d_gather) + 16 (d_fieldA/B) + 8 (d_indicesA/B) + 1 (d_flags) = 73 bytes
    size_t expandBytes = (size_t)batchSize * (size_t)(1 + maxMovesPerBoard) * sizeof(BOARD_KEY)
                       + (size_t)batchSize * sizeof(int)
                       + sizeof(uint32_t);
    size_t budget      = totalGpuBytes * 8 / 10;
    if (budget > expandBytes) budget -= expandBytes;
    p->accumCapacity   = budget / 73;

    GPU_CHECK(cudaStreamCreate(&p->stream));

    // Per-batch device buffers
    GPU_CHECK(cudaMalloc(&p->d_input,    (size_t)batchSize * sizeof(BOARD_KEY)));
    GPU_CHECK(cudaMalloc(&p->d_results,  (size_t)batchSize * (size_t)maxMovesPerBoard * sizeof(BOARD_KEY)));
    GPU_CHECK(cudaMalloc(&p->d_counts,   (size_t)batchSize * sizeof(int)));
    GPU_CHECK(cudaMalloc(&p->d_writePos, sizeof(uint32_t)));

    // Pinned host buffers for async transfers
    GPU_CHECK(cudaMallocHost(&p->h_input,    (size_t)batchSize * sizeof(BOARD_KEY)));
    GPU_CHECK(cudaMallocHost(&p->h_writePos, sizeof(uint32_t)));

    // Large device buffers
    GPU_CHECK(cudaMalloc(&p->d_accum,    p->accumCapacity * sizeof(BOARD_KEY)));
    GPU_CHECK(cudaMalloc(&p->d_gather,   p->accumCapacity * sizeof(BOARD_KEY)));
    GPU_CHECK(cudaMalloc(&p->d_fieldA,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_fieldB,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesA, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesB, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_flags,      p->accumCapacity * sizeof(uint8_t)));
    GPU_CHECK(cudaMalloc(&p->d_batchStats, 3 * sizeof(uint32_t)));
    GPU_CHECK(cudaMemset( p->d_batchStats, 0, 3 * sizeof(uint32_t)));
    p->h_batchStats[0] = 0;
    p->h_batchStats[1] = 0;
    p->h_batchStats[2] = 0;

    // Pre-allocate CUB temp storage for the sort and prefix-sum used every flush
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

    GPU_CHECK(cudaMemset(p->d_writePos, 0, sizeof(uint32_t)));
    p->writeOffset = 0;
    p->uniqueCount = 0;
    return p;
}

void GpuAccumulatorDestroy(GpuAccumulator* pAccum)
{
    cudaStreamSynchronize(pAccum->stream);
    cudaStreamDestroy(pAccum->stream);
    cudaFree(pAccum->d_input);
    cudaFree(pAccum->d_results);
    cudaFree(pAccum->d_counts);
    cudaFree(pAccum->d_writePos);
    cudaFreeHost(pAccum->h_input);
    cudaFreeHost(pAccum->h_writePos);
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
    return (pAccum->writeOffset + worstCase) <= pAccum->accumCapacity;
}

void GpuProcessBatch(GpuAccumulator* pAccum, const BOARD_KEY* pBoards, int count)
{
    cudaStream_t s = pAccum->stream;

    memcpy(pAccum->h_input, pBoards, (size_t)count * sizeof(BOARD_KEY));
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_input, pAccum->h_input,
                              (size_t)count * sizeof(BOARD_KEY),
                              cudaMemcpyHostToDevice, s));

    // Set scatter counter to current write offset before expansion
    uint32_t wo = (uint32_t)pAccum->writeOffset;
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_writePos, &wo, sizeof(uint32_t),
                              cudaMemcpyHostToDevice, s));

    // Phase 1: expand — one thread per input board
    {
        int threads = 256;
        int blocks  = (count + threads - 1) / threads;
        ExpandKernel<<<blocks, threads, 0, s>>>(
            pAccum->d_input, pAccum->d_results, pAccum->d_counts,
            count, pAccum->maxMovesPerBoard,
            pAccum->boardConsts, pAccum->numRotations,
            pAccum->d_batchStats);
    }

    // Phase 2: scatter valid slots into d_accum
    {
        int total   = count * pAccum->maxMovesPerBoard;
        int threads = 256;
        int blocks  = (total + threads - 1) / threads;
        ScatterKernel<<<blocks, threads, 0, s>>>(
            pAccum->d_results, pAccum->d_counts,
            count, pAccum->maxMovesPerBoard,
            pAccum->d_accum, (uint32_t)pAccum->accumCapacity,
            pAccum->d_writePos);
    }

    GPU_CHECK(cudaMemcpyAsync(pAccum->h_writePos, pAccum->d_writePos, sizeof(uint32_t),
                              cudaMemcpyDeviceToHost, s));
    GPU_CHECK(cudaStreamSynchronize(s));

    pAccum->writeOffset = *pAccum->h_writePos;
}

// ============================================================================
// GpuFlushPrepare
//
// 1. 3-pass CUB radix sort (LSB-first: f2->f1->f0) producing d_indicesA.
// 2. Mark duplicate flags via adjacent comparison on the permutation.
// 3. Invert flags and exclusive-sum to get output positions.
// 4. Compact unique boards (in sorted order) into d_gather.
//
// After this call, d_gather[0..uniqueCount-1] contains the unique boards in
// ascending sort order, ready for GpuFlushRead.
// ============================================================================

int GpuFlushPrepare(GpuAccumulator* pAccum)
{
    uint32_t N = (uint32_t)pAccum->writeOffset;
    if (N > (uint32_t)pAccum->accumCapacity)
        N = (uint32_t)pAccum->accumCapacity;
    if (N == 0)
    {
        // Stream is already synced from the end of GpuProcessBatch, so d_batchStats
        // is final. Copy it now so callers see pass/terminal/maxMoves even on an
        // all-terminal level where no child boards were generated.
        GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                             3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
        pAccum->uniqueCount = 0;
        return 0;
    }

    int threads = 256;
    int blocks  = ((int)N + threads - 1) / threads;

    // ----- 3-pass stable radix sort (reuse pre-allocated temp buffer) -----
    void*  d_temp    = pAccum->d_sortTemp;
    size_t tempBytes = pAccum->sortTempBytes;

    // Pass 1: sort by f2 (bytes 16-23)
    ExtractFieldKernel<<<blocks, threads>>>(pAccum->d_accum, N, 2, pAccum->d_fieldA);
    InitIndicesKernel <<<blocks, threads>>>(pAccum->d_indicesA, N);
    {
        cub::DoubleBuffer<uint64_t> kDb(pAccum->d_fieldA, pAccum->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(pAccum->d_indicesA, pAccum->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(pAccum->d_indicesA, pAccum->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // Pass 2: sort by f1 (bytes 8-15), gathered through current permutation
    GatherFieldKernel<<<blocks, threads>>>(pAccum->d_accum, pAccum->d_indicesA, N, 1, pAccum->d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> kDb(pAccum->d_fieldA, pAccum->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(pAccum->d_indicesA, pAccum->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(pAccum->d_indicesA, pAccum->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // Pass 3: sort by f0 (bytes 0-7, MSB field)
    GatherFieldKernel<<<blocks, threads>>>(pAccum->d_accum, pAccum->d_indicesA, N, 0, pAccum->d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> kDb(pAccum->d_fieldA, pAccum->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(pAccum->d_indicesA, pAccum->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(pAccum->d_indicesA, pAccum->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // ----- Dedup: mark adjacent duplicates -----
    MarkDupFlagsKernel<<<blocks, threads>>>(pAccum->d_accum, pAccum->d_indicesA, pAccum->d_flags, N);

    // ----- Compact: exclusive-sum then gather into d_gather -----
    // Reuse d_fieldA (uint64_t array) as a uint32_t not-flags buffer.
    // The cast is safe: d_fieldA has accumCapacity uint64_t slots = 2*accumCapacity
    // uint32_t slots, which is more than enough for N not-flag values.
    uint32_t* d_notFlags = reinterpret_cast<uint32_t*>(pAccum->d_fieldA);
    InvertFlagsKernel<<<blocks, threads>>>(pAccum->d_flags, d_notFlags, N);

    // Exclusive prefix-sum of not-flags -> output positions in d_indicesB
    size_t scanBytes = pAccum->scanTempBytes;
    void*  d_scan    = pAccum->d_scanTemp;
    GPU_CHECK((cub::DeviceScan::ExclusiveSum(d_scan, scanBytes, d_notFlags, pAccum->d_indicesB, (int)N)));

    // Scatter unique boards in sorted order into d_gather
    CompactKernel<<<blocks, threads>>>(
        pAccum->d_accum, pAccum->d_indicesA, pAccum->d_flags, pAccum->d_indicesB, N, pAccum->d_gather);
    GPU_CHECK(cudaDeviceSynchronize());

    // uniqueCount = last prefix-sum value + last not-flag value
    uint32_t h_lastPos;  uint32_t h_lastNotFlag;
    GPU_CHECK(cudaMemcpy(&h_lastPos,     pAccum->d_indicesB + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
    GPU_CHECK(cudaMemcpy(&h_lastNotFlag, d_notFlags         + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));

    pAccum->uniqueCount = (int)(h_lastPos + h_lastNotFlag);

    // D2H pass/terminal/maxMoves counts accumulated across all batches in this flush window
    GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                         3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));

    return pAccum->uniqueCount;
}

// ============================================================================
// GpuFlushRead
//
// D2H a chunk of the sorted+deduped output from d_gather.
// GpuFlushPrepare must have been called first.
// Each call issues one cudaMemcpy covering exactly [offset, offset+got).
// ============================================================================

int GpuFlushRead(GpuAccumulator* pAccum, size_t offset, BOARD_KEY* pOut, int maxCount)
{
    int avail = pAccum->uniqueCount - (int)offset;
    if (avail <= 0) return 0;
    int got = (avail < maxCount) ? avail : maxCount;
    GPU_CHECK(cudaMemcpy(pOut, pAccum->d_gather + offset,
                         (size_t)got * sizeof(BOARD_KEY), cudaMemcpyDeviceToHost));
    return got;
}

// ============================================================================
// GpuFlushReset
// ============================================================================

void GpuFlushReset(GpuAccumulator* pAccum)
{
    GPU_CHECK(cudaMemsetAsync(pAccum->d_writePos,    0, sizeof(uint32_t),     pAccum->stream));
    GPU_CHECK(cudaMemsetAsync(pAccum->d_batchStats,  0, 3 * sizeof(uint32_t), pAccum->stream));
    pAccum->writeOffset     = 0;
    pAccum->uniqueCount     = 0;
    pAccum->h_batchStats[0] = 0;
    pAccum->h_batchStats[1] = 0;
    pAccum->h_batchStats[2] = 0;
}

size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum)
{
    return pAccum->writeOffset;
}

uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum)
{
    return pAccum->h_batchStats[0];
}

uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum)
{
    return pAccum->h_batchStats[1];
}

uint32_t GpuFlushMaxMoves(const GpuAccumulator* pAccum)
{
    return pAccum->h_batchStats[2];
}
