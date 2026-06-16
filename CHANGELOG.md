# Changelog

All notable changes to OthelloLevelBlaster are documented here.

---

## [0.1.8] - 2026-06-16

### Added
- **Solve-phase progress in status table** — The current-level row in the status
  history table now shows `[solve XX.X%]` during GPU solving instead of the
  generic `[running]` tag.  The GPU feeder pre-scans the input BLF file(s) via
  `BLFTrailer(r)->recordCount` (trailer-only seek, cheap even on NAS) before
  reading starts and stores the total in `currentLevelTotalBoards`.
  `StatsListener` divides `boardsReadFromStore` by this total for the percentage.
  The merge phase already showed `[merge XX.X%]`; both percentages now work.

---

## [0.1.7] - 2026-06-16

### Fixed
- **Column overflow in main log** — Board counts at level 16+ reach 11–12 digits,
  overflowing the previous `%10llu`/`%8llu`/`%7.2f` format specs in
  `PrintLevelStatsHeader` and `LogLevelSummary`.  All board-count columns widened
  to `%14llu`, GB columns to `%9.2f`, and timing columns to `%10.3f`/`%12llu`.
  Header and separator strings updated to match.

- **Column overflow in status table** — Same problem in `StatsListener.cpp`;
  format specs widened to `%13llu`/`%12llu`/`%9.2f`.  Header and separator
  strings updated to match.

### Added
- **Current-level row in status table** — The history section in
  `BuildStatusResponse` previously showed only completed levels.  It now appends a
  live in-progress row for the level currently running, with a phase tag in the
  `ns/brd` column: `[solving]`, `[flushing]`, `[merge XX.X%]`, or `[done]`.
  History loop starts from `resumeLevel` so no blank rows appear on resume.

---

## [0.1.6] - 2026-06-16

### Fixed
- **Off-by-one in resume level** — `ScanForResumeLevel` returns the index of the
  first *missing* store file (e.g. 18 when Level_0017 exists but Level_0018 does
  not).  But iteration N *reads* Level_N and *writes* Level_N+1, so the correct
  restart point is `firstMissingFile - 1` (17 in the example), not
  `firstMissingFile` (18).  Starting at 18 would make the GPU feeder try to open
  Level_0018 which does not exist.  Fixed at the call site:
  `resumeLevel = (firstMissingFile > 0) ? firstMissingFile - 1 : 0`.

---

## [0.1.5] - 2026-06-16

### Fixed
- **Resume skips incomplete level files** — `ScanForResumeLevel` previously used
  `GetFileAttributesA` (existence-only check), so a level file truncated by a
  mid-write crash would be treated as complete.  It now opens each candidate with
  `BLFOpen`, which validates the `BLF_MAGIC` trailer.  If the magic is absent the
  file is deleted and that level becomes the resume point, so the solver re-runs
  it cleanly rather than feeding corrupt data to the GPU feeder.

---

## [0.1.4] - 2026-06-16

### Fixed
- **Stale defaults in `--help` output** — `--board-size`, `--drives`, and
  `--store-drive` still showed the 0.1.0 defaults (4, DEFZ, Z) after those were
  changed to 6, DEFY, Y in 0.1.1.

### Added
- **Auto-resume description in `--help`** — explains that the solver detects
  existing level files and resumes automatically, and that deleting storeDir
  manually triggers a fresh run.

---

## [0.1.3] - 2026-06-16

### Added
- **Automatic resume** — On startup, `InitSolver` scans `storeDir` for
  `Level_XXXX_file_0000.bin` files and sets `resumeLevel` to the first missing
  level.  If `resumeLevel > 0`, the store directory is preserved (not purged)
  and the level loop starts from `resumeLevel` instead of 0.  `CreateSeedFile`
  already no-ops when `Level_0000_file_0000.bin` exists, so no seed-file change
  is needed.  The completed-level history table at run end starts from
  `resumeLevel` so zeroed skipped-level rows are not printed.
- **`storeMergeFileCount` reset per level** — The atomic intermediate-merge
  fallback counter on the store drive (added in 0.1.2) is now reset to 0 at the
  start of each level, consistent with `mergeFileCount[]`.

---

## [0.1.2] - 2026-06-16

### Fixed
- **Intermediate merge space overflow** — `DoIntermediateMerge` previously called
  `CascadingMerge` on all writer files at once.  For large levels (e.g. level 17:
  361 files / 3.7 TB) this triggers a 2-level cascade that needs up to
  `2 × inputBytes` of scratch space on the medium drive simultaneously (group temp
  files + final output all coexist).  That can exceed the medium drive's capacity
  and eventually cause a `Fatal(FATAL_DRIVE_SPACE, ...)`.

  **Fix**: `DoIntermediateMerge` now splits files into batches of `MAX_MERGE_FANIN`
  and calls `KWayMergeFiles` once per batch.  Each batch is a single-pass merge
  with no cascade temp files, so worst-case space = batch input bytes (1×, not 2×).
  Multiple batches are routed to whichever medium drive has room; if no medium drive
  can fit a batch, that batch is written to a new **store merge directory** on the
  store drive (Y:).

- **Intermediate merge file-counter race** — Two MW threads (D: and E:) can call
  `DoIntermediateMerge` simultaneously and both write to the same medium merge dir,
  racing on `mergeFileCount[i]++`.  All counter increments now use
  `InterlockedExchangeAdd` to guarantee unique filenames without a mutex.

### Added
- **Store merge directory** (`Y:\…\storeMergeDir`) — created and purged alongside
  the existing directories.  Acts as last-resort intermediate merge destination
  when no medium drive has sufficient free space for a batch.  `DoEndOfLevelMerge`
  scans it automatically alongside the medium merge dirs.

- **Per-file size tracking in `EnumerateWriterFiles`** — optional `outSizes` array
  parameter (defaults to `nullptr`; existing callers unchanged) so
  `DoIntermediateMerge` can compute accurate per-batch byte counts without
  re-opening each file.

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
