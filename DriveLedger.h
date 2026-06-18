#pragma once
// Per-drive space ledger.
//
// Tracks available bytes via our own write/delete accounting instead of
// repeated OS queries.  Initialized from the OS once after cleanup (and
// re-initialized at each level start to correct any accumulated drift).
// A flat DRIVE_SPACE_LOW_BYTES safety buffer is reserved at init time so
// that no reservation can ever consume the last 20 GB on any drive — this
// accounts for OS filesystem overhead, MFT growth, etc.
//
//   DriveInitLedger  — query OS, subtract safety buffer, store as baseline
//   DriveReserve     — CAS-loop subtract; false (no change) if insufficient
//   DriveReclaim     — unconditional add (file deleted or overestimate returned)
//   DriveDebit       — unconditional subtract (actual bytes written; single-writer paths)
//   DriveAvailable   — read current value (display / threshold checks)

#include "OthelloTypes.h"
#include <windows.h>

// Query the OS, subtract the 20 GB safety buffer, and store as the drive's
// available baseline.  Call after cleanup, before any writes begin for a level.
static inline void DriveInitLedger(POthelloLevelBlasterState pSt, char letter)
{
    char root[4] = { letter, ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};
    GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);
    int64_t available = (int64_t)freeAvail.QuadPart - (int64_t)DRIVE_SPACE_LOW_BYTES;
    if (available < 0) available = 0;
    InterlockedExchange64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)available);
}

// Atomically reserve bytes on a drive.
// Subtracts from the ledger only if available >= bytes (CAS loop).
// Returns true on success, false (ledger unchanged) if insufficient space.
static inline bool DriveReserve(POthelloLevelBlasterState pSt, char letter, int64_t bytes)
{
    volatile LONG64* p =
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')];
    LONG64 old = InterlockedCompareExchange64(p, 0, 0);
    for (;;)
    {
        if (old < (LONG64)bytes) return false;
        LONG64 got = InterlockedCompareExchange64(p, old - (LONG64)bytes, old);
        if (got == old) return true;
        old = got;
    }
}

// Return bytes to the ledger (file deleted or overestimate corrected).
static inline void DriveReclaim(POthelloLevelBlasterState pSt, char letter, int64_t bytes)
{
    InterlockedAdd64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)bytes);
}

// Unconditional subtract (actual bytes written; for single-writer paths where
// no pre-reserve is needed because only one thread touches the drive).
static inline void DriveDebit(POthelloLevelBlasterState pSt, char letter, int64_t bytes)
{
    InterlockedAdd64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        -(LONG64)bytes);
}

// Read the current available bytes (for display or threshold comparison).
static inline int64_t DriveAvailable(POthelloLevelBlasterState pSt, char letter)
{
    return (int64_t)InterlockedCompareExchange64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')], 0, 0);
}
