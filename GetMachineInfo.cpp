#include "GetMachineInfo.h"

void getGpuInformation(PGpuInformation pGpuInfo)
{
    GetGpuInformation(pGpuInfo);
    PrintGpuInformation(pGpuInfo);
}

void getSystemMemoryInfoBudget(PMemoryInfo pMemInfo)
{
    pMemInfo->requestedMode = MM_RECOMMENDED;
    pMemInfo->requestedBytes = 0; // not used for MM_RECOMMENDED

    CalcMemoryBudget(pMemInfo);

    // Print the results
    char physStr[64], availStr[64], budgetStr[64];
    sizeToGBString(pMemInfo->totalPhys, physStr, sizeof(physStr));
    sizeToGBString(pMemInfo->availPhys, availStr, sizeof(availStr));
    sizeToGBString(pMemInfo->budgetedSize, budgetStr, sizeof(budgetStr));
    printf("Total Physical RAM     : %s\n", physStr);
    printf("Available Physical RAM : %s\n", availStr);
    printf("Memory Budget          : %s\n", budgetStr);
}

void getDriveInformation(char *pCacheDir, char *pDriveStr, PMachineDriveInfo driveInfo)
{
    GetDriveInformation(driveInfo, pCacheDir, pDriveStr);
    PrintDriveInformation(driveInfo);
}

void GetMachineInfo(char* pCacheDir, char *pDriveStr, PMachineInfo pMachineInfo)
{
    getSystemMemoryInfoBudget(&pMachineInfo->g_memInfo);
    getDriveInformation(pCacheDir, pDriveStr,&(pMachineInfo->g_drives));
    getGpuInformation(&(pMachineInfo->g_gpuInfo));
}
