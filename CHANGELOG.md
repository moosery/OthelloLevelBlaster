# Changelog

All notable changes to OthelloLevelBlaster are documented here.

---

## [0.2.13] - 2026-06-21

### Distribute cascade temp files across F: and Y: instead of all-or-nothing (`MergeFiles.cpp`)

Previously `DoEndOfLevelMerge` reserved temp space for all cascade groups on one
drive before any merge thread started: if F: had room for the full player input, all
temps went to F:; otherwise all temps went to Y: (NAS).  At L19, F: was already
occupied by imerge files and the other player's groups, so black's 12 cascade groups
all fell to Y: (91 MB/s write, 68 MB/s read) instead of the faster F: (188 MB/s).

**New behaviour:** cascade temp placement is decided per-group inside `CascadingMerge`
using the same `DriveReserve` / `DriveReclaim` ledger.  A prioritized list of candidate
dirs is passed in (`mergeDirectory[]` first, `storeMergeDirectory` last); each group
picks the first dir with ledger space at the moment it starts.  When F: has room for
some groups and Y: only for others, the temps spread automatically.

Key effects:
- **Phase 1 (write temps):** groups on F: write at 188 MB/s instead of 91 MB/s,
  reducing total Phase 1 time roughly proportional to the fraction that fits on F:.
- **Phase 2 (final k-way merge):** reads all temp files simultaneously, so temps split
  across F: and Y: yield concurrent I/O (~188 + 68 = 256 MB/s combined read) instead
  of sequential reads from a single slower drive.
- **No upfront all-or-nothing reservation:** the ledger pressure on both drives is
  spread in time, which may allow more groups on F: than the upfront check would have
  allowed.

`CascadingMerge` signature change: `const char* workDir` replaced by
`const char** tempDirs, int numTempDirs`.  Temp-file reclaim now uses
`tempPaths[i][0]` (drive letter from the actual path) instead of a single
`workDir[0]`, correctly handling temps on different drives.

Each group's drive allocation is logged:
```
CascadingMerge: black group 1/12 -> F: (312.45 GB input)
CascadingMerge: black group 7/12 -> Y: (298.12 GB input)
```

---

## [0.2.12] - 2026-06-21

### Fix history table column overflow at L19+ (`StatsListener.cpp`)

`GpuDups` and `MrgDups` columns used `%12llu` (12-char field); at L19 GPU dups
reach ~2.4 trillion (13 digits), overflowing the field and misaligning all
subsequent columns.  `Generated` at `%13llu` was also on the edge (L20 will
exceed it).

All five board-count columns widened:

| Column | Old | New |
|--------|-----|-----|
| BoardsIn | `%13llu` | `%14llu` |
| Generated | `%13llu` | `%15llu` |
| GpuDups | `%12llu` | `%14llu` |
| MrgDups | `%12llu` | `%14llu` |
| Written | `%13llu` | `%14llu` |
| SlvGB | `%9.2f` | `%10.2f` |

Header and separator lines updated to match.

---

## [0.2.11] - 2026-06-20

### Display: per-player merge progress, store file count, wider ns/brd, drive table with Uncomp GB (`OthelloTypes.h`, `MergeFiles.cpp`, `OthelloLevelBlaster.cpp`, `StatsListener.cpp`)

**Per-player merge progress** — `mergeProgressBytes` and `mergeTotalInputBytes` are now
`[2]` arrays indexed by `BLF_PLAYER_WHITE(0)` / `BLF_PLAYER_BLACK(1)`.  The per-player
`pProg` pointer is set inside the `mergePlayer` lambda so each player thread writes to
its own slot.  The status client current-level row shows `[W: 57%/B: 55%]` instead of
the previous combined percentage, giving independent progress for each player's merge.

**Correct Y: store file count** — `LevelStats` gains `mergeFilesWritten` (incremented
via `InterlockedIncrement` after each player writes output; typically 2 = black + white).
The blaster-log Y: drive line now prints `mergeFilesWritten` instead of
`ls->mergeBytes > 0 ? 1 : 0`, which always showed 1.

**Wider ns/brd column** — history table `%8llu` widened to `%10llu` (with matching
header and separator) so 9-digit values at early levels no longer overflow the column.

**Drive table Uncomp GB column** — the status client drive breakdown adds an `Uncomp GB`
column alongside `Disk GB` when compression is active, so compressed vs. uncompressed
size is visible at a glance.  Header, separator, and data rows are re-aligned for
consistent column widths.

**Merge progress line removed** — the separate "Merge progress" line above the level
history table is gone; the `[W:xx%/B:xx%]` percentage now appears inline in the current
level's table row, keeping everything in one aligned block.

---

## [0.2.10] - 2026-06-19

### Fix merge progress percentage > 100% under compression (`MergeFiles.cpp`)

`mergeTotalInputBytes` was set from `EnumerateByPattern` which returns actual
bytes on disk.  For compressed `.blfz` inputs those bytes are 3–8× smaller than
the uncompressed record payload.  `mergeProgressBytes` is incremented by
`sizeof(BOARD_KEY_DISK)` (16 bytes) per record — uncompressed units — so the
percentage could reach 170%+ at typical compression ratios.

Fix: after enumerating all input files for both players, open each file briefly
(reads the 64-byte trailer only) to sum `recordCount * 16`, then replace
`mergeTotalInputBytes` with that uncompressed-equivalent value.  Progress and
total are now in the same units regardless of compression mode.

---

## [0.2.9] - 2026-06-19

### Fast Ctrl+C termination; NVMe drive lines show compressed vs uncompressed (`MergeFiles.cpp`, `OthelloTypes.h`, `OthelloLevelBlaster.cpp`)

**Fast shutdown on Ctrl+C** — previously terminating mid-merge could take minutes
because the merge loops ran to completion with no terminate check.  Now:
- `KWayMergeFiles` checks `terminateThreads` at the top of every heap iteration
  and drains open readers cleanly on early exit.
- `CascadingMerge` checks before starting each group merge so no new groups begin.
- `DoIntermediateMerge` checks before each 256-file batch.
- `FlushMergeWriterBuffer` checks in both in-memory heap loops.
- GPU feeder already checked the flag in its inner loop; no change needed there.

Ctrl+C → full stop in under a second regardless of level size.

**NVMe drive lines now show compressed vs. uncompressed** — `WriterDriveStats`
gains `levelBytesUncompressed`; `FlushMergeWriterBuffer` tracks both the actual
bytes written and the uncompressed equivalent.  `LogLevelSummary` shows the same
`X.XX GB on disk  (Y.YY GB uncompressed equiv)` format as the Y: store line when
`COMPRESS_ALL` is active.

---

## [0.2.8] - 2026-06-19

### Compress all files by default (`OthelloTypes.h`, `BlasterFile.h/.cpp`, `BlasterFileName.h`, `MergeFiles.cpp`, `OthelloLevelBlaster.cpp`, `InitSolver.cpp`)

Extends delta+varint compression from store-only output to **every** BLF file the
solver writes: merge-writer (NVMe) files, intermediate merge files (F:), cascade
temp files, and final store files (Y:).

**New compression mode flag** (`uint8_t compressMode` replaces `bool compressStoreFiles`):
- `--compress` (default) — all files written as `.blfz`
- `--compress-store-only` — only Y: store output compressed; MW/imerge stay `.blf`
- `--no-compress` — all files uncompressed `.blf`

**Drive space accounting is now accurate** under compression:
- `BLFWriterClose` gains an optional `pFileBytes` out-parameter that returns the
  actual bytes written (compressed payload + trailer), so `DriveDebit` in
  `FlushMergeWriterBuffer` debits the real compressed size rather than the
  uncompressed estimate.  With smaller NVMe debits the ledger correctly reflects
  that drives have more room, so intermediate merge triggers later — more boards
  fit on NVMe before spilling to F:.
- `DoIntermediateMerge` and `CascadingMerge` use `GetFileAttributesExA` on the
  output file for the `DriveReclaim` calculation when compression is active.
- `DriveReserve` always uses worst-case (input size) upfront; the reclaim corrects
  the overestimate after the write — this safe pattern is unchanged.

**File enumeration** in `DoIntermediateMerge` and `DoEndOfLevelMerge` now probes
both `*.blf` and `*.blfz` patterns when `COMPRESS_ALL` is active, so a mid-level
resume after a mode change still picks up all existing files.

**`CascadingMerge`** gains a `compressIntermediate` parameter; when true the
intermediate group temp files are also written as `.blfz`, and actual temp sizes
are measured via `GetFileAttributesExA` for accurate `DriveReclaim` calls.

---

## [0.2.7] - 2026-06-19

### Show actual vs. uncompressed store size in stats (`OthelloTypes.h`, `MergeFiles.cpp`, `OthelloLevelBlaster.cpp`)

Added `mergeActualBytes` to `LevelStats` — populated via `GetFileAttributesExA`
on the output file(s) after each end-of-level merge.

- **`MrgGB` column** now shows actual bytes on disk (compressed when `.blfz`).
- **Y: per-drive line** shows both when compression is active:
  `X.XX GB on disk  (Y.YY GB uncompressed equiv)  free=Z.ZZ GB`
- Uncompressed runs show the same single-value format as before.

---

## [0.2.6] - 2026-06-19

### Fix GPU feeder not finding .blfz store files (`LevelSolverThread.cpp`)

`EnumerateStoreFilesForLevel` only probed the `*.blf` glob pattern, so when
compression was active and the end-of-level merge wrote `*.blfz` files, the
next level's GPU feeder found zero input boards and silently produced no output
— cascading to every subsequent level also producing nothing.

Same two-line fallback as `ScanForResumeLevel`: if `*.blf` finds nothing, try
`*.blfz`. `BLFOpen` handles magic dispatch transparently after the file is found.

---

## [0.2.5] - 2026-06-18

### Add 1 MB read buffer to BLFOpen (`BlasterFile.cpp`)

Uncompressed readers had no `setvbuf` call, so the CRT used its default 4 KB
buffer.  `KWayMergeFiles` reads one 16-byte record at a time from the heap, so
the CRT was issuing tiny reads and seeking between concurrent file positions on
every heap pop.  On spinning drives (F: cascade temps) this killed throughput.

Added `setvbuf(f, NULL, _IOFBF, BLF_COMP_READ_BUFFER_SIZE)` (1 MB) for
uncompressed readers immediately after the struct is initialised.  Each reader
now pulls 1 MB sequentially before repositioning, reducing seeks by ~256x.

---

## [0.2.4] - 2026-06-18

### Default to compressed store files

`compressStoreFiles` now defaults to `true`; use `--no-compress` to revert to
uncompressed `.blf`.  Help text updated to reflect the new default.

---

## [0.2.3] - 2026-06-18

### Compressed store files (`--compress`, `.blfz` format)

Added optional delta+zigzag+varint compression for store files, reducing on-disk
size by ~3.5x (varint alone) compared to uncompressed `.blf`.

**New flag**: `--compress` writes store files as `.blfz`; `--no-compress` keeps
the default uncompressed `.blf`.  The format is chosen once at startup and applies
to all end-of-level output files.  Intermediate/temp files (writer, imerge, cascade)
always stay `.blf`.

- **`BlasterFile.h`** — `BLFZ_MAGIC`, `BLF_COMP_WRITE_BUFFER_SIZE`,
  `BLF_COMP_READ_BUFFER_SIZE`, `BLFWriterOpenZ` declaration.
- **`BlasterFile.cpp`** — streaming delta+zigzag+varint writer
  (`BLFWriterOpenZ`, 64 KB write buffer, `FlushVarBuf`); streaming reader
  (`BLFZReadByte`, `BLFZReadVarInt`, 1 MB read buffer); `BLFOpen` dispatches
  on magic value so callers need no format knowledge; `BLFClose` frees the
  read buffer; compressed byte count stored in `_reserved[0..7]` of the trailer.
- **`BlasterFileName.h`** — `BLFZNameStoreFile`, `BLFZPatternStoreFiles`,
  `BLFZPatternAnyStoreFiles` for `.blfz` naming.
- **`OthelloTypes.h`** — `compressStoreFiles` field in config.
- **`OthelloLevelBlaster.cpp`** — `--compress` / `--no-compress` flags;
  startup log line showing selected format.
- **`MergeFiles.cpp`** — `KWayMergeFiles` and `CascadingMerge` accept a
  `compressed` flag; `DoEndOfLevelMerge` chooses `.blfz` filename and
  `BLFWriterOpenZ` when flag is set; Y: drive reclaim uses actual file size
  (via `GetFileAttributesExA`) for compressed output to correctly account for
  the compression savings.
- **`InitSolver.cpp`** — `ScanForResumeLevel` probes `.blfz` pattern when no
  `.blf` is found; `BLFOpen` handles magic detection transparently.

---

## [0.2.2] - 2026-06-18

### Drive space ledger (`DriveLedger.h`, `MergeFiles.cpp`, `LevelSolverThread.cpp`, `StatsListener.cpp`)

Previously, every space decision queried `GetDiskFreeSpaceExA` at the moment of
use.  This had three failure modes: (1) the parallel black/white end-of-level
merge threads could both see "F: has room" when only one fit; (2) the cascade
space check underestimated peak usage because input files and temp files coexist
on F: simultaneously; (3) the stats display triggered OS calls on every poll.

**The fix** replaces all ad-hoc OS queries with a single per-drive `volatile
int64_t driveLedger[26]` atomic counter in `OthelloLevelBlasterState`.

- **`DriveLedger.h`** (new) — four operations: `DriveInitLedger` (query OS
  once, subtract 20 GB safety buffer, store as baseline), `DriveReserve`
  (CAS-loop subtract; returns false if insufficient — no side-effect on failure),
  `DriveReclaim` (add back on file deletion or overestimate correction),
  `DriveDebit` (unconditional subtract for single-writer NVMe paths).
- **20 GB safety buffer**: every `DriveInitLedger` call subtracts
  `DRIVE_SPACE_LOW_BYTES` (20 GB) from the OS-reported free bytes so that no
  reservation can ever consume the last 20 GB — providing headroom for filesystem
  metadata, MFT growth, and other OS overhead.
- **`SelectMergeDestination` removed**: the old function used a parallel
  `mergeDirReservedBytes[]` array for F: reservation but ignored D:, E:, and Y:.
  All decisions now go through `DriveReserve` / `DriveReclaim`.
- **`FlushMergeWriterBuffer`**: `DriveDebit(NVMe, fileBytes)` after writing;
  threshold check reads `DriveAvailable` instead of a live OS query.
- **`DoIntermediateMerge`**: `DriveReserve(F_or_Y, batchBytes)` before each
  batch (try all merge dirs in order, fall back to Y:, Fatal if none fit);
  `DriveReclaim(dest, batchBytes - actual)` for dedup savings; `DriveReclaim`
  per deleted source file.
- **`CascadingMerge`**: per-group input bytes summed via `GetFileAttributesExA`;
  `DriveReclaim(tempDrive, groupBytes - tempActual)` after each group; per-temp
  `DriveReclaim` when temps are deleted.
- **`DoEndOfLevelMerge` Phase 1b** (new, runs before threads start): atomically
  reserves cascade temp space on F: for **both** players before either thread
  starts — eliminating the race where both threads each saw "F: has room" but
  combined they overflowed it.  Falls back to Y: per player if F: is full.
  Also pre-reserves Y: worst-case output space for each player; `DriveReclaim`
  corrects the overestimate after each merge completes.
- **`StatsListener`**: drive free-space display for D:/E:, F:, and Y: now reads
  from the ledger — no OS calls on the stats poll path.
- Ledgers are initialised after startup cleanup and **re-initialised at the
  start of every level** so any accumulated drift is corrected with a single
  fresh OS query per drive per level.

---

## [0.2.0] - 2026-06-17

This release is a complete architectural overhaul of the on-disk format, GPU
accumulator, and file pipeline.  **On-disk files from 0.1.x are incompatible
with this version** (different record size and filename convention).

### Why

The 0.1.x pipeline stored 24-byte `BOARD_KEY` structs on disk even though 8 of
those bytes — `usBoardInfo` (2 bytes) and `_pad[6]` — were derivable from the
filename alone (board size is in the name; player turn is now the
`_black_` / `_white_` filename segment).  Keeping them on disk wasted I/O,
inflated BLF file sizes, and forced the GPU accumulator to carry a redundant
intermediate `d_results` buffer (~456 MB on typical parameters) and a separate
`ScatterKernel` pass.

The fix is to make the record 16 bytes everywhere — GPU accumulator, MW buffer,
merge heap, BLF files — and encode the only non-derivable context (player turn)
in the filename.

### Breaking changes

- **On-disk record size**: `BOARD_KEY_DISK` is 16 bytes (`ullCellsInUse` +
  `ullCellColors`).  The old 24-byte `BOARD_KEY` is no longer written to disk.
- **Filename convention**: level files are now named
  `Level_NNNN_SxS_black_NNNN.blf` / `Level_NNNN_SxS_white_NNNN.blf`;
  intermediate files are `writer_black_NNNN.blf` / `writer_white_NNNN.blf`.
  Old `Level_NNNN_file_NNNN.bin` naming is gone.

### Changed — GPU accumulator (`GpuKernels.cu / .h`)

- **Two-stack layout**: black boards grow from index 0 upward via
  `atomicAdd(d_blackWritePos,1)`; white boards grow from `accumCapacity-1`
  downward via `atomicAdd(d_whiteWritePos,1)`.  A single `BOARD_KEY_DISK*
  d_accum` array holds both players simultaneously.
- **`ExpandKernel` scatters directly to `d_accum`**: child boards are written
  straight to the two-stack accumulator with no intermediate buffer.
  `ScatterKernel` and the `d_results` array (~456 MB) are eliminated entirely.
- **`GpuFlushPrepare` runs two independent sort+dedup passes** via
  `SortAndDedupRegion`: one on the black region (`d_accum[0..B-1]`), one on
  the white region (`d_accum[cap-W..cap-1]`).  Results are placed in `d_gather`
  (black first, then white).
- **`GpuFlushRead(pAccum, player, offset, pOut, maxCount)`**: reads from
  `d_gather` with a per-player base offset; caller can D2H each player
  independently.
- **`GpuFlushBlackCount` / `GpuFlushWhiteCount`** — new accessors.
- **Memory budget**: 57 bytes/slot (`16+16+8+8+4+4+1`) vs the old 73
  bytes/slot — ~32% more accumulator capacity on the same GPU.

### Changed — MW buffer and merge writer (`LevelSolverThread.cpp`, `OthelloTypes.h`)

- **Two-stack MW buffer**: mirrors the GPU layout.  Black segments grow from
  the start of the buffer; white segments grow inward from the end.  Eight
  per-thread fields in `OthelloLevelBlasterState` replace the four old
  single-stream fields: `mwBlackSegCount/Offset/Size/BoardsUsed[]`,
  `mwWhiteSegCount/Offset/Size/BoardsUsed[]`.
- **`FlushDescriptor`** carries `blackCount` + `whiteCount` (was a single
  `uniqueCount`).

### Changed — merge pipeline (`MergeFiles.cpp`)

- **`FlushMergeWriterBuffer`** runs two independent k-way in-memory merges:
  one over black segments, one over white, producing separate
  `writer_black_NNNN.blf` / `writer_white_NNNN.blf` files.
- **`InMemDiskHead` / `InMemDiskHeadGreater`** replace the old `InMemHead`
  (which referenced 24-byte `BOARD_KEY`); comparator uses `ullCellsInUse`
  then `ullCellColors` directly.
- **`DoEndOfLevelMerge`** processes black and white streams independently,
  producing two per-level store files.

### Changed — GPU feeder (`LevelSolverThread.cpp`)

- **Two sub-passes per level**: pass 1 reads all `_black_` store files with
  `playerBit = BLF_PLAYER_BLACK`; pass 2 reads all `_white_` store files with
  `BLF_PLAYER_WHITE`.  No mid-level accumulator flush between passes is
  required because the MW buffer's two-stack layout can absorb both.

### Changed — BLF layer (`BlasterFile.h / .cpp`)

- All read/write APIs use `BOARD_KEY_DISK*` (16 bytes) exclusively.
- `BlasterFileTrailer.minKey / maxKey` are 16-byte arrays.
- `BLF_MAGIC`, `BLF_WRITE_BUFFER_SIZE`, and `BLF_PLAYER_BLACK/WHITE` constants.

### Changed — seed file (`CreateSeedFile.cpp`)

- Seed written as a 16-byte `BOARD_KEY_DISK` record with the standard starting
  position; file named `Level_0000_SxS_black_0000.blf` (black plays first).

### Changed — end-of-level merge parallelism (`MergeFiles.cpp`)

- **Black and white merges now run concurrently** — `DoEndOfLevelMerge` was a
  sequential loop over `{white, black}`.  It is now two-phase:
  1. Sequential scan: enumerate input files for both players and set
     `mergeTotalInputBytes` *before* any merge starts, so the stats display
     has an accurate denominator from the first progress update.
  2. Parallel merge: one `std::thread` per player.  Each thread opens at most
     `MAX_MERGE_FANIN (256)` files simultaneously; peak simultaneous
     file-descriptor use is therefore 514 — comfortably within the 2048
     `_setmaxstdio` ceiling.
- **`mergeProgressBytes` is now `volatile int64_t`** (was `uint64_t`),
  incremented via `InterlockedAdd64` so both merge threads can update it
  concurrently without tearing.  The stats listener reads it as a volatile
  load (no lock needed — it's a best-effort UI percentage).
- **`KWayMergeFiles` / `CascadingMerge`** now take `volatile int64_t*`
  for the progress counter; the cast to `volatile LONG64*` is confined to
  the two `InterlockedAdd64` call sites.

### Changed — stats display (`StatsListener.cpp`)

- Drive table now shows `BlkLive` and `WhtLive` columns — live count of
  writer files on that drive for each player — so black/white write balance
  is visible in real time.

### Fixed

- **Per-level reset used deleted field names**: `OthelloLevelBlaster.cpp` still
  referenced `mwFileCount[]`, `mergeFileCount[]`, `storeMergeFileCount` which
  were replaced by `mwBlackFileCount`/`mwWhiteFileCount` etc. in previous
  sessions.  Corrected to the six new split field names.
- **Stale `levelFileNamePattern` macro**: `LevelSolverThread.h` contained
  `#define levelFileNamePattern "Level_%04d_file_%04d"` pointing to the old
  `.bin` naming scheme.  Removed.
- **Missing `#include "Error.h"` in `BlasterFile.cpp`**: `Fatal` and
  `FATAL_FILE_OPEN` / `FATAL_ALLOCATION_FAILED` were not declared because the
  Utility header was not pulled in.  Fixed.

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
