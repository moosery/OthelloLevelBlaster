#include "GpuInfo.h"
#include "Logger.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CUDA_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) { \
            fprintf(stderr, "CUDA error at %s:%d  %s\n", \
                    __FILE__, __LINE__, cudaGetErrorString(_e)); \
            exit(1); \
        } \
    } while (0)

// ============================================================================
// GetGpuInformation
// ============================================================================

void GetGpuInformation(GpuInformation* pInfo)
{
    memset(pInfo, 0, sizeof(*pInfo));

    int deviceIndex = 0;
    CUDA_CHECK(cudaGetDevice(&deviceIndex));
    pInfo->deviceIndex = deviceIndex;

    cudaDeviceProp p = {};
    CUDA_CHECK(cudaGetDeviceProperties(&p, deviceIndex));

    strncpy(pInfo->name, p.name, sizeof(pInfo->name) - 1);
    pInfo->computeCapabilityMajor = p.major;
    pInfo->computeCapabilityMinor = p.minor;
    pInfo->smCount                = p.multiProcessorCount;
    pInfo->maxThreadsPerSM        = p.maxThreadsPerMultiProcessor;
    pInfo->warpSize               = p.warpSize;
    pInfo->totalGlobalMemBytes    = p.totalGlobalMem;
    pInfo->l2CacheSizeBytes       = p.l2CacheSize;
    pInfo->asyncEngineCount       = p.asyncEngineCount;

    // Optimal batch: fill all SMs with 256-thread blocks, capped at 64K.
    int sat = p.multiProcessorCount * (p.maxThreadsPerMultiProcessor / 256) * 256;
    pInfo->optimalBatchSize = (sat < 65536) ? sat : 65536;

    // Worker count: 2 per async engine, clamped to [2, 8].
    int w = p.asyncEngineCount * 2;
    if (w < 2) w = 2;
    if (w > 8) w = 8;
    pInfo->recommendedWorkerCount = w;
}

// ============================================================================
// PrintGpuInformation
// ============================================================================

void PrintGpuInformation(const GpuInformation* p)
{
    double vramGB = (double)p->totalGlobalMemBytes / (1024.0 * 1024.0 * 1024.0);
    double l2MB   = (double)p->l2CacheSizeBytes    / (1024.0 * 1024.0);

    LoggerLog("GPU [%d]: %s  (sm_%d%d)\n",
           p->deviceIndex, p->name,
           p->computeCapabilityMajor, p->computeCapabilityMinor);
    LoggerLog("  SMs                : %d  x  %d threads/SM  (warp=%d)\n",
           p->smCount, p->maxThreadsPerSM, p->warpSize);
    LoggerLog("  VRAM               : %.0f GB\n", vramGB);
    LoggerLog("  L2 Cache           : %.0f MB\n", l2MB);
    LoggerLog("  Async Engines      : %d\n", p->asyncEngineCount);
    LoggerLog("  Optimal Batch Size : %d\n", p->optimalBatchSize);
    LoggerLog("  Worker Count       : %d\n", p->recommendedWorkerCount);
}
