#pragma once
#include "OthelloBasics.h"  // BOARD_KEY

// Opaque GPU accumulator — full definition is internal to GpuKernels.cu.
struct __GpuAccumulator;
typedef struct __GpuAccumulator GpuAccumulator;

// Create/destroy the accumulator.  Allocates device memory (80% of totalGpuBytes).
GpuAccumulator* GpuAccumulatorCreate(int batchSize, int maxMovesPerBoard, size_t totalGpuBytes);
void            GpuAccumulatorDestroy(GpuAccumulator* pAccum);

// Returns true if the accumulator has room for another batch of nextBatchCount boards
// (pessimistic check: assumes every board produces maxMovesPerBoard children).
bool GpuAccumulatorHasRoom(const GpuAccumulator* pAccum, int nextBatchCount);

// H2D copy of count boards + async expansion kernel launch.
// Caller must ensure HasRoom() before calling.
void GpuProcessBatch(GpuAccumulator* pAccum, const BOARD_KEY* pBoards, int count);

// Sync stream, sort + dedup all accumulated expansion output on device.
// Returns the unique board count ready for GpuFlushRead; 0 if nothing to flush.
int GpuFlushPrepare(GpuAccumulator* pAccum);

// D2H copy of [offset, offset+maxCount) from the sorted+deduped result.
// GpuFlushPrepare must have been called first.  Returns count actually copied.
int GpuFlushRead(GpuAccumulator* pAccum, size_t offset, BOARD_KEY* pOut, int maxCount);

// Reset the accumulator for the next accumulation window.
// Must be called after all GpuFlushRead calls for a given flush are done.
void GpuFlushReset(GpuAccumulator* pAccum);

// Returns the raw board count accumulated since the last GpuFlushReset
// (pre-dedup; used for boardsGenerated stats tracking).
size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum);

// Pass and terminal board counts accumulated across all GpuProcessBatch calls
// in the current flush window.  Valid only after GpuFlushPrepare returns;
// zeroed by GpuFlushReset.
uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum);
uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum);
uint32_t GpuFlushMaxMoves(const GpuAccumulator* pAccum);
