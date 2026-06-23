#pragma once
#include "LevelSolverThread.h"

// In-memory k-way merge of accumulated GPU flush segments for merge-writer
// thread thdIdx.  Streams the sorted+deduped result directly to a BLF file
// on that thread's NVMe directory, then resets the segment tracking.
// Called by the merge-writer job when the buffer is full, and by
// FlushAllMergeWriterBuffers at end of level.
void FlushMergeWriterBuffer(int thdIdx, PSolveContext pCtx);

// Called from the main level loop after all merge-writer buffers have been
// flushed.  Consolidates every remaining writer file (NVMe) and intermediate
// merge file (HDD) into a single sorted, deduped store file on the store drive.
// Output: storeDirectory\Level_XXXX_file_0000.bin
void DoEndOfLevelMerge(PSolveContext pCtx);
