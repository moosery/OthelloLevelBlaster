# OthelloLevelBlaster

GPU-accelerated BFS enumeration of unique Othello (Reversi) board states by level.

Each *level* corresponds to one piece placed on the board (starting from 4 pre-placed pieces).
The solver expands all legal moves from every unique board at level N, canonicalizes each
result under the 16-element D4 × color-swap symmetry group, and deduplicates to produce
the set of unique boards at level N+1.  The output for each level is a pair of sorted binary
files on the store drive — one for black-to-move boards and one for white-to-move boards.

## Architecture

```
Store drive (Y:)
  Level_N_black.blfz ──┐
  Level_N_white.blfz ──┘  (or .blf when --no-compress)
         │
    GPU feeder (ping-pong buffer, one batch at a time)
         │
    ExpandKernel → two-stack GPU accumulator (black ↑ from 0, white ↓ from cap)
         │
    GpuFlushPrepare: CUB DeviceRadixSort + dedup (two independent passes)
         │
    D2H to merge-writer thread (one per NVMe directory)
         │
    In-memory k-way merge + dedup per player → writer_black_NNNN.blfz / writer_white_NNNN.blfz
         │                                      (on D:, E:, ...; .blf with --no-compress)
         │  [if NVMe low: DoIntermediateMerge → imerge files on F:]
         │
    DoEndOfLevelMerge (parallel: black thread + white thread)
      ├─ black: CascadingMerge(all black writer + imerge files) → Level_(N+1)_black_0000.blfz
      └─ white: CascadingMerge(all white writer + imerge files) → Level_(N+1)_white_0000.blfz
                                                                    (both on Y:)
```

### Key components

- **GPU feeder** — reads boards from the store drive in batches via a ping-pong buffer and
  feeds the GPU accumulator.  Two sub-passes per level: black-to-move files first, then
  white-to-move files.
- **GPU accumulator (two-stack)** — a single `BOARD_KEY_DISK*` array holds both players:
  black grows from index 0 upward, white grows from `capacity-1` downward.  `ExpandKernel`
  scatters children directly to the accumulator (no intermediate buffer).  Flushed to
  merge-writer threads when VRAM is ~80% full.
- **Merge-writer threads** — one per fast NVMe directory.  Accumulate GPU flush segments
  in a large RAM buffer (two-stack layout mirroring the GPU), k-way merge + dedup per
  player on flush, write separate `_black_` and `_white_` BLF files.  After each flush,
  check two triggers for `DoCrossDriveIntermediateMerge`: (1) total unconsumed writer
  files per color across all NVMe drives ≥ `MAX_MERGE_FANIN`; (2) single-drive free
  space below 20 GB threshold (safety net).
- **Intermediate merge** — `DoCrossDriveIntermediateMerge` gathers unconsumed writer
  files from **all** NVMe drives simultaneously (not per-drive), catching cross-drive
  duplicates during the solve.  Up to `MAX_MERGE_FANIN=3500` files per color are merged
  to a single imerge output on F:.  If F: is full, a **total flush** occurs: existing F:
  imerge files for this level are also pulled in, and the combined set is written to Y:
  in one shot — clearing all fast drives at once so F: can be reused.  Only one MW
  thread runs a merge at a time (guarded by `imergeCS`); the other skips via
  `TryEnterCriticalSection`.
- **End-of-level merge** — two threads run concurrently (one per player), each performing
  a `CascadingMerge` over all remaining writer files and intermediate files.  If per-player
  file count exceeds 256, a two-phase cascade is used: each group's temp is placed on the
  first drive with ledger space (F: preferred, Y: as fallback), so temps spread across drives
  rather than all going to one.  This lets Phase 2 read temps concurrently from both drives
  (~188 + 68 = 256 MB/s) instead of sequentially from the slower one.
- **Drive space ledger** — all space decisions (intermediate merge destination, cascade temp
  drive, final output) use an atomic per-drive ledger (`driveLedger[26]` in
  `OthelloLevelBlasterState`) rather than live OS queries.  The ledger is seeded from the
  OS after startup cleanup and re-seeded at each level start, with a 20 GB safety buffer
  subtracted so reservations never reach the last bytes on any drive.

## Requirements

| Component | Minimum |
|-----------|---------|
| OS        | Windows 10/11 x64 |
| Compiler  | Visual Studio 2022 with CUDA toolkit |
| GPU       | NVIDIA sm_89 (RTX 40-series) — change `sm_89` in vcxproj for other architectures |
| RAM       | 40 GB recommended (scales with VRAM and number of NVMe drives) |
| Fast drives | 1–4 NVMe drives for merge-writer working space |
| Intermediate drive | HDD/SATA SSD for overflow merge (F: by default) |
| Store drive | Large slow drive for level output files (Y: by default, 15+ TB recommended for 6×6) |

## Build

Open `OthelloLevelBlaster.slnx` in Visual Studio 2022 and build the solution in **Release | x64**.

Outputs:
- `x64/Release/OthelloLevelBlaster.exe` — main solver
- `x64/Release/OthelloLevelBlasterStatus.exe` — live status client (TCP)

## Usage

```
OthelloLevelBlaster.exe [options]

  --board-size N         Board size (e.g. 4 for 4x4, 6 for 6x6)      [default: 6]
  --drives LETTERS       Drive letters to use, e.g. DEFY               [default: DEFY]
  --store-drive L        Drive letter for store output                 [default: Y]
  --store-dir PATH       Sub-path on store drive (no drive letter)     [default: \OthelloLevelBlaster\Store]
  --cache-dir PATH       Full path for logs and drive-bench cache      [default: C:\OthelloLevelBlaster\Cache]
  --port N               Stats listener TCP port                       [default: 17432]
  --compress             Compress all files as .blfz (delta+varint)    [default]
  --compress-store-only  Compress only store (Y:) output; MW/imerge stay .blf
  --no-compress          Write all files as .blf (uncompressed)
  --help                 Show this help
```

While the solver runs, query live status from another terminal:

```
OthelloLevelBlasterStatus.exe
```

The status client shows current level progress, per-drive free space (from the ledger),
merge progress percentage, cascade progress, and a completed-level history table.

Auto-resume: if the store directory already contains level files from a previous run,
the solver automatically resumes from the first missing level.

Press **Ctrl+C** for a graceful shutdown — all merge loops check the terminate flag
and stop within a second.  Partial output files are cleaned up automatically on the
next run.

### Drive layout convention

| Drive | Role | Notes |
|-------|------|-------|
| D:, E: | Fast (NVMe) | Merge-writer working space; needs room for one full level's writer output |
| F: | Intermediate (HDD/SATA) | Overflow intermediate merge + cascade temp files |
| Y: | Store (NAS/large HDD) | Accumulates two sorted BLF files per level (black + white); needs total of all level output |

Drives are auto-detected and categorized by benchmark speed.  The cache file
`C:\OthelloLevelBlaster\Cache\driveinfo.json` stores benchmark results across runs.

## File format

Board states are stored in **BLF** (Blaster Level File) or **BLFZ** (compressed) format.

**BLF** (uncompressed):
- A sorted array of `BOARD_KEY_DISK` records, **16 bytes each**:
  `uint64_t ullCellsInUse` + `uint64_t ullCellColors`.
- A 64-byte `BlasterFileTrailer` at the end: magic `BLSTFILE`, record count, min/max key.

**BLFZ** (delta+zigzag+varint compressed, default):
- Delta-encoded then zigzag+varint compressed payload — typically 3–8× smaller than BLF.
- Same 64-byte trailer with magic `BLSTFILZ`; `_reserved[0..7]` stores the compressed byte count.
- `BLFOpen` dispatches on the magic value, so readers handle both formats transparently.
- Compression ratio improves at higher levels as board positions become denser in key-space.

Files are sorted in ascending numeric order matching CUB DeviceRadixSort output.
Player turn (black-to-move / white-to-move) is encoded in the filename, not the record.

### Filename conventions

| Pattern | Description |
|---------|-------------|
| `Level_NNNN_SxS_black_0000.blfz` | Level N black-to-move store file on Y: (compressed) |
| `Level_NNNN_SxS_white_0000.blfz` | Level N white-to-move store file on Y: (compressed) |
| `Level_NNNN_SxS_black_0000.blf` | Same, uncompressed (--no-compress) |
| `writer_black_NNNN.blfz` | In-progress flush output from a merge-writer thread |
| `writer_white_NNNN.blfz` | In-progress flush output from a merge-writer thread |
| `imerge_LNNN_black_NNNN.blfz` | Intermediate merge output on F: |
| `cascade_temp_LNNN_black_NNNN.blfz` | Cascade temp file on F: (deleted after use) |

## Performance (6×6, RTX 4080 SUPER, v0.2.20)

*Measured run: RTX 4080 SUPER, 64 GB RAM, D:/E: NVMe, F: HDD (188 MB/s), Y: NAS (~59 MB/s write).
Timing for L12–L17 from v0.2.11 run; L18–L20 from v0.2.10 run.
Y: (cumul.) = total logical file size on Y: at end of that level (all levels L0–N), measured from current store files.
ZFS transparent compression on the NAS pool gives ~2.1× additional savings on top of the .blfz application compression,
so physical disk usage is roughly half the values shown.*

| Level | Unique boards | Solve    | Merge    | Total    | Y: (cumul.) |
|-------|--------------|----------|----------|----------|-------------|
| 12    | 251 M        | 5.9 s    | 12.9 s   | 18.8 s   | 0.31 GB     |
| 13    | 1.21 B       | 23.6 s   | 57.8 s   | 81.4 s   | 1.46 GB     |
| 14    | 5.01 B       | 1.8 min  | 4.7 min  | 6.4 min  | 6.67 GB     |
| 15    | 19.8 B       | 5.8 min  | 19.9 min | 25.8 min | 27.1 GB     |
| 16    | 65.9 B       | 22.2 min | 71.1 min | 93.3 min | 104 GB      |
| 17    | 203.5 B      | 68.5 min | 6.4 h    | 7.6 h    | 349 GB      |
| 18    | 526 B        | 3.3 h    | 16.5 h   | 19.8 h   | 1.05 TB     |
| 19    | 1.235 T      | 9.0 h    | 49.4 h   | 58.4 h   | 2.84 TB     |
| 20    | ~2.82 T      | —        | —        | —        | 6.90 TB     |

Y: NAS write speed (~59 MB/s sustained) dominates merge time through L16.  At L17 the
cascade merge through F: HDD kicks in, causing merge time to jump from 71 min (L16) to
6.4 h (L17).  At L19 (v0.2.10), D: and E: accumulated 3056 and 3080 writer files — the
end-of-level cascade overflowed F: and spilled cascade temps to Y:.  v0.2.15 raises
`MAX_MERGE_FANIN` to 3500 (from 256) and adds a cross-drive intermediate merge, which
will substantially reduce cascade overhead at L20+.
Compression ratio is ~3.3× at L12, ~4.2× at L17–L19.

## Project layout

```
OthelloLevelBlaster/
  OthelloLevelBlaster.cpp      Main loop, argument parsing, level driver
  OthelloTypes.h               Shared structs: config, state, stats, driveLedger
  DriveLedger.h                Per-drive atomic space ledger (Reserve/Reclaim/Debit)
  InitSolver.cpp / .h          Resource allocation, drive setup, cleanup, ledger init
  GpuKernels.cu / .h           CUDA move expansion, canonical form, two-stack accumulator
  GpuInfo.cu / .h              GPU device query
  LevelSolverThread.cpp / .h   Merge-writer job + GPU feeder thread (ping-pong reader → GPU)
  MergeFiles.cpp / .h          FlushMergeWriterBuffer, DoIntermediateMerge,
                                CascadingMerge, DoEndOfLevelMerge
  BlasterFile.cpp / .h         BLF reader/writer (16-byte records + 64-byte trailer)
  BlasterFileName.h            BLF filename construction and pattern helpers
  StatsListener.cpp / .h       TCP stats server (port 17432)
  CreateSeedFile.cpp / .h      Level-0 seed file generator
  GetMachineInfo.cpp / .h      Drive detection, GPU detection, benchmarking
  InitLogger.cpp / .h          Log file setup

  OthelloBasics/               Board representation, move generation, canonicalization (CPU)
  OthelloBasicsForCUDA/        Same, compiled for device code
  BPlusTreeHybrid/             B+ tree (currently unused in main path)
  Utility/                     Threading, memory, clocks, drive info, logging

  OthelloLevelBlasterStatus/   Status client project
```
