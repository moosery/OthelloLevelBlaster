#pragma once
#include "BlasterFile.h"   // BOARD_KEY_DISK

// Opaque GPU accumulator — full definition is internal to GpuKernels.cu.
struct __GpuAccumulator;
typedef struct __GpuAccumulator GpuAccumulator;

// Create/destroy the accumulator.  Allocates device memory (80% of totalGpuBytes).
GpuAccumulator* GpuAccumulatorCreate(int batchSize, int maxMovesPerBoard, size_t totalGpuBytes);
void            GpuAccumulatorDestroy(GpuAccumulator* pAccum);

// Returns true if the accumulator has room for another batch of nextBatchCount boards
// (pessimistic: assumes every board produces maxMovesPerBoard children).
bool GpuAccumulatorHasRoom(const GpuAccumulator* pAccum, int nextBatchCount);

// H2D copy of count BOARD_KEY_DISK boards + async expand + direct two-stack scatter.
// playerBit: BLF_PLAYER_BLACK (1) or BLF_PLAYER_WHITE (0) for this batch.
// Caller must ensure HasRoom() before calling.
void GpuProcessBatch(GpuAccumulator* pAccum, const BOARD_KEY_DISK* pBoards,
                     int count, uint8_t playerBit);

// Sync stream, sort+dedup both stack regions on device.
// Returns total unique board count (black + white); 0 if nothing to flush.
// After this call, GpuFlushBlackCount() and GpuFlushWhiteCount() are valid.
int GpuFlushPrepare(GpuAccumulator* pAccum);

// D2H chunk from the sorted+deduped result for one player.
// player: BLF_PLAYER_BLACK reads d_gather[0..blackUnique-1];
//         BLF_PLAYER_WHITE reads d_gather[blackUnique..blackUnique+whiteUnique-1].
// GpuFlushPrepare must have been called first.  Returns count actually copied.
int GpuFlushRead(GpuAccumulator* pAccum, int player, size_t offset,
                 BOARD_KEY_DISK* pOut, int maxCount);

// Number of unique black / white boards produced by the last GpuFlushPrepare.
int GpuFlushBlackCount(const GpuAccumulator* pAccum);
int GpuFlushWhiteCount(const GpuAccumulator* pAccum);

// Reset the accumulator for the next accumulation window.
// Must be called after all GpuFlushRead calls for a flush are done.
void GpuFlushReset(GpuAccumulator* pAccum);

// Total raw boards accumulated since last GpuFlushReset (pre-dedup; both stacks).
size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum);

// Pass / terminal / max-move counts accumulated across GpuProcessBatch calls
// in the current flush window.  Valid only after GpuFlushPrepare; zeroed by GpuFlushReset.
uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum);
uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum);
uint32_t GpuFlushMaxMoves(const GpuAccumulator* pAccum);
