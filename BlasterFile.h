#pragma once
#include <stdint.h>
#include "OthelloBasics.h"

// ============================================================================
// Blaster file format
//
// Every file produced or consumed by OthelloLevelBlaster uses this layout:
//
//   [ BOARD_KEY record 0 ]
//   [ BOARD_KEY record 1 ]
//   ...
//   [ BOARD_KEY record N-1 ]
//   [ BlasterFileTrailer   ]   <-- 64 bytes, always at file end
//
// Records are sorted ascending (ullCellsInUse, then ullCellColors, then
// usBoardInfo) and contain no duplicates.
//
// The trailer is written last.  A missing or corrupt magic means the file was
// never fully written (crash / partial flush) and must be discarded.
// ============================================================================

#define BLF_MAGIC 0x424C535446494C45ULL   // "BLSTFILE" in little-endian ASCII

#pragma pack(push, 1)
typedef struct __BlasterFileTrailer
{
    uint8_t  minKey[24];    // first BOARD_KEY in sorted order
    uint8_t  maxKey[24];    // last  BOARD_KEY in sorted order
    uint64_t recordCount;   // BOARD_KEY records preceding this trailer
    uint64_t magic;         // BLF_MAGIC — written last; absence = incomplete file
} BlasterFileTrailer, *PBlasterFileTrailer;
#pragma pack(pop)

static_assert(sizeof(BlasterFileTrailer) == 64, "BlasterFileTrailer must be 64 bytes");

// Write count sorted BOARD_KEY records to path, followed by the trailer.
// Records must already be sorted and deduped.  Fatals on any I/O error.
void BLFWrite(const char* path, const BOARD_KEY* pBoards, uint64_t count);

// Streaming writer — records are fed one at a time (e.g. from a k-way merge).
// The CRT write buffer is set to BLF_WRITE_BUFFER_SIZE bytes.
// BLFWriterClose writes the trailer, closes the file, and returns the record count.
#define BLF_WRITE_BUFFER_SIZE (512 * 1024)

typedef struct __BLFWriter BLFWriter;

BLFWriter* BLFWriterOpen(const char* path);
void       BLFWriterRecord(BLFWriter* pw, const BOARD_KEY* pKey);
uint64_t   BLFWriterClose(BLFWriter* pw);

// Opaque sequential reader.
typedef struct __BLFReader BLFReader;

// Open a blaster file for sequential reading.
// Validates the trailer (magic + size sanity) before returning.
// Returns nullptr if the file is missing, incomplete, or corrupt — does NOT fatal.
BLFReader* BLFOpen(const char* path);

// Read up to maxCount BOARD_KEY records into pOut.
// Returns count actually read; 0 = EOF.
int BLFRead(BLFReader* r, BOARD_KEY* pOut, int maxCount);

// Trailer from an open reader (valid until BLFClose).
const BlasterFileTrailer* BLFTrailer(const BLFReader* r);

void BLFClose(BLFReader** ppReader);
