# Changelog

All notable changes to OthelloLevelBlaster are documented here.

---

## [0.1.1] - 2026-06-15

### Fixed
- **MrgDups=0 bug** — `BoardKeyCompare` was using `memcmp`, which on little-endian x86
  compares bytes LSB-first.  CUB DeviceRadixSort produces ascending numeric uint64 order,
  which is the opposite byte direction.  The k-way merge heap therefore saw files as
  unsorted and failed to detect duplicates across writer threads.  Fixed by replacing
  `memcmp` with explicit numeric `uint64_t` field comparisons.  Verified: level-12
  `UniqueOut` now matches OLE reference exactly (251,087,725).

- **Ctrl+C hang** — After Ctrl+C, the main loop still ran `FlushAllMergeWriterBuffers`
  (up to 20 GB buffer flush) and `DoEndOfLevelMerge` (hours for large levels) before
  exiting.  Both are now skipped when `terminateThreads` is set.

- **Stats listener dying on Ctrl+C** — The listener loop was gated on `terminateThreads`,
  so it exited the moment Ctrl+C was pressed, making the status tool unusable during
  shutdown.  Replaced with a separate `terminateStatsListener` flag set only in
  `CleanupSolver` after all worker pools have stopped.

### Added
- **Phase display in status** — `currentPhase` field on state; set to `"GPU solving"`,
  `"Flushing buffers"`, or `"Merging to store"` at each transition.  Status client shows
  the current phase instead of the generic `[RUNNING]` tag.

- **Merge progress** — During `"Merging to store"`, status now shows bytes written vs
  total input bytes with a percentage:
  `Merge progress : 3.21 / 5.99 GB  (53.6%)`
  Progress counter is threaded through `KWayMergeFiles` / `CascadingMerge` and only
  tracks the final output pass (not intermediate cascade passes).

- **Store drive in per-level stats** — End-of-level log line now includes a drive entry
  for the store drive (free space after merge completes).

### Changed
- Default store drive changed from `Z:` to `Y:` (15.68 TB, 59 MB/s write vs Z:'s 20 MB/s).
- Default `--drives` changed from `DEFZ` to `DEFY` to match.
- Default board size changed from 4×4 to 6×6.

### Internal
- `WriterDriveStats` struct moved before `LevelStats` in `OthelloTypes.h` to fix
  forward-declaration error.
- Added `#include <stdint.h>` to `OthelloBasics/BoardKeyCompare.cpp` for `uint64_t`.
- Version bumped from `0.1.0` to `0.1.1`.

---

## [0.1.0] - 2026-06-14

Initial working release.

### Features
- GPU-accelerated BFS Othello board enumeration using CUDA (sm_89, RTX 4080 SUPER).
- CUB `DeviceRadixSort` for in-GPU dedup within each accumulation window.
- Ping-pong buffer reader feeding GPU from store drive.
- Multi-threaded merge-writer: one thread per NVMe directory, large RAM accumulation
  buffers, in-memory k-way merge + dedup on each flush.
- Intermediate merge (NVMe → HDD) triggered when NVMe free space drops below threshold.
- End-of-level k-way cascading merge to a single sorted output file on the store drive.
- Drive auto-detection, benchmarking, and categorization (Fast/Moderate/NAS).
- TCP stats listener on port 17432; companion `OthelloLevelBlasterStatus.exe` client.
- Per-level stats: boards in/out, GPU dups, merge dups, solve/merge/total times, drive breakdown.
- Ctrl+C handler for graceful shutdown (sets `terminateThreads`).
- `_setmaxstdio(2048)` to support up to `MAX_MERGE_FANIN=256` simultaneous open files.
- 4×4 board counts verified against OLE (OthelloLevelEnumerator) reference.
- 6×6 level-12 unique board count verified against OLE (251,087,725) after MrgDups fix.
