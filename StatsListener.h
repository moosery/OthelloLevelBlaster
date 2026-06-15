#pragma once
#include "LevelSolverThread.h"

// Submits a long-running job to pState->pStatsThreadPool that listens for
// STATUS and STOP commands on pConfig->statsPort.  Returns immediately.
void SubmitStatsListenerJob(PSolveContext pCtx);
