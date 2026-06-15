#pragma once
#include "Utility.h"
#include "GpuInfo.h"

typedef struct __MachineInfo
{
    MemoryInfo g_memInfo = {};
    MachineDriveInfo g_drives = {};
    GpuInformation g_gpuInfo = {};
} MachineInfo, * PMachineInfo;

void GetMachineInfo(char* pCacheDir, char* pDriveStr, PMachineInfo pMachineInfo);