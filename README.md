# OthelloLevelBlaster

GPU-accelerated BFS enumeration of unique Othello (Reversi) board states by level.

Each *level* corresponds to one piece placed on the board (starting at 4 pre-placed pieces).
The solver expands all legal moves from every unique board at level N, canonicalizes each
result under the 16-element D4 × color-swap symmetry group, and deduplicates to produce
the set of unique boards at level N+1.  The output for each level is a single sorted binary
file stored on the designated store drive.

## Architecture

```
Store drive (Y:)                   NVMe drives (D:, E:)           GPU
  Level_N.bin ──ping-pong──► GPU feeder ──expand──► GPU accumulator
                                                          │
                                              CUB DeviceRadixSort (sm_89)
                                                          │
                                        merge-writer threads (one per NVMe dir)
                                           in-memory k-way merge + dedup
                                                          │
                                              writer files on D:/E:
                                                          │
                                           DoEndOfLevelMerge (k-way)
                                                          │
                                          Level_(N+1).bin on store drive
```

- **GPU feeder** — reads boards from the store drive in batches (ping-pong buffer) and
  feeds the GPU accumulator.
- **GPU accumulator** — performs move expansion and CUB DeviceRadixSort-based dedup
  within each accumulation window.  Flushed to merge-writer threads when VRAM is ~80% full.
- **Merge-writer threads** — one per fast NVMe directory.  Accumulate GPU flush segments
  in a large RAM buffer, k-way merge + dedup on flush, write a sorted binary BLF file.
  Trigger intermediate merge to the HDD intermediate drive (F:) when NVMe free space is low.
- **End-of-level merge** — k-way merge of all writer files and intermediate merge files
  into a single sorted, deduped output file on the store drive.

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

  --board-size N    Board size (e.g. 4 for 4x4, 6 for 6x6)  [default: 6]
  --drives LETTERS  Drive letters to use, e.g. DEFY           [default: DEFY]
  --store-drive L   Drive letter for store output             [default: Y]
  --store-dir PATH  Sub-path on store drive (no drive letter) [default: \OthelloLevelBlaster\Store]
  --cache-dir PATH  Full path for logs and drive-bench cache  [default: C:\OthelloLevelBlaster\Cache]
  --port N          Stats listener TCP port                   [default: 17432]
  --help            Show this help
```

While the solver runs, query live status from another terminal:

```
OthelloLevelBlasterStatus.exe
```

The status client shows current level progress, per-drive throughput, merge progress
percentage, and a completed-level history table.

### Drive layout convention

| Drive | Role | Notes |
|-------|------|-------|
| D:, E: | Fast (NVMe) | Merge-writer working space — needs ~2× level solve GB free |
| F: | Intermediate (HDD) | Overflow merge destination when NVMe fills |
| Y: | Store (NAS) | Accumulates one sorted file per level; needs total of all level files |

Drives are auto-detected and categorized by benchmark speed.  The cache file
`C:\OthelloLevelBlaster\Cache\driveinfo.json` stores benchmark results across runs.

## Performance (6×6, RTX 4080 SUPER)

| Level | Unique boards | Solve time | Merge time | Total |
|-------|--------------|------------|------------|-------|
| 12    | 251 M        | 16.7 s     | 63.6 s     | 80.3 s |
| 13    | 1.21 B       | 68.5 s     | 308 s      | 377 s  |
| 14    | 5.01 B       | 233 s      | 1323 s     | 1556 s |

Store drive Y: (59 MB/s write, PCIe 1).  Merge speed scales with store drive bandwidth.

## File format

Board states are stored in **BLF** (Blaster Level File) format: a sorted array of
`BOARD_KEY` records (24 bytes each) followed by a fixed-size trailer with record count
and checksum.  Files are always sorted in ascending numeric order matching the GPU's
CUB DeviceRadixSort output (uint64 numeric order on each 8-byte field, LSB-field first).

## Project layout

```
OthelloLevelBlaster/
  OthelloLevelBlaster.cpp      Main loop, argument parsing, level driver
  OthelloTypes.h               Shared structs: config, state, stats
  InitSolver.cpp / .h          Resource allocation, drive setup, cleanup
  GpuKernels.cu / .h           CUDA move expansion, canonical form, accumulator
  GpuInfo.cu / .h              GPU device query
  LevelSolverThread.cpp / .h   GPU feeder thread (ping-pong reader → GPU)
  MergeFiles.cpp / .h          Flush, intermediate merge, end-of-level merge
  BlasterFile.cpp / .h         BLF reader/writer
  SortAndDedup.cpp / .h        In-memory sort+dedup helpers
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
