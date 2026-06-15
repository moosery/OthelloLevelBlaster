#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct __GpuInformation {
    // --- Identity ---
    int  deviceIndex;
    char name[256];
    int  computeCapabilityMajor;
    int  computeCapabilityMinor;

    // --- Compute ---
    int smCount;              // streaming multiprocessor count
    int maxThreadsPerSM;      // max resident threads per SM
    int warpSize;

    // --- Memory ---
    size_t totalGlobalMemBytes;
    int    l2CacheSizeBytes;

    // --- Concurrency ---
    int asyncEngineCount;     // hardware DMA copy engines

    // --- Derived ---
    int optimalBatchSize;        // boards per GPU batch to saturate SMs
    int recommendedWorkerCount;  // suggested CPU worker threads (from async engine count)
} GpuInformation, *PGpuInformation;

// Query the first CUDA device and fill pInfo.
// Calls exit(1) with a message on any CUDA error.
void GetGpuInformation(GpuInformation* pInfo);

// Print a summary of GPU information to stdout.
void PrintGpuInformation(const GpuInformation* pInfo);
