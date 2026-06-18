#pragma once
#include "OthelloTypes.h"
#include "OthelloBasics.h"
#include "GpuKernels.h"
#include "GetMachineInfo.h"
#include <windows.h>

#define PING_PONG_SLOTS         4

// Passed from the GPU feeder to a merge-writer pool job.
// hDoneEvent is signaled by the merge-writer after both D2H ops complete so
// the GPU feeder can reset the accumulator and start the next batch immediately.
typedef struct __FlushDescriptor
{
    GpuAccumulator* pAccum;
    int             blackCount;   // unique black boards in this flush
    int             whiteCount;   // unique white boards in this flush
    HANDLE          hDoneEvent;   // auto-reset; signaled after D2H is done
} FlushDescriptor, *PFlushDescriptor;

typedef struct __SolveContext
{
    POthelloLevelBlasterConfig  pConfig;
    POthelloLevelBlasterState   pState;
    PMachineInfo                pMachineInfo;
} SolveContext, *PSolveContext;

void SubmitGpuFeederJob(PSolveContext pCtx, uint8_t level);
void SubmitMergeWriterJob(PSolveContext pCtx, PFlushDescriptor pDesc);
void FlushAllMergeWriterBuffers(PSolveContext pCtx);
