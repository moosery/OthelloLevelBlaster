#pragma once
#include "OthelloTypes.h"
#include "GetMachineInfo.h"

void InitSolver(POthelloLevelBlasterConfig pConfig, POthelloLevelBlasterState pState, PMachineInfo pMachineInfo);
void CleanupSolver(POthelloLevelBlasterState pState);
