#pragma once
#include <stdint.h>

// ============================================================================
// Blaster file format  (16-byte record edition)
//
// Every file produced or consumed by OthelloLevelBlaster uses this layout:
//
//   [ BOARD_KEY_DISK record 0 ]   -- 16 bytes each
//   [ BOARD_KEY_DISK record 1 ]
//   ...
//   [ BOARD_KEY_DISK record N-1 ]
//   [ BlasterFileTrailer         ]  -- 64 bytes, always at file end
//
// Records contain only the two bitboard fields (ullCellsInUse + ullCellColors).
// Player turn and board size are encoded in the filename, not the record.
// Records are sorted ascending (ullCellsInUse first, then ullCellColors) and
// contain no duplicates within a player stream.
//
// The trailer is written last.  A missing or corrupt magic means the file was
// never fully written (crash / partial flush) and must be discarded.
// ============================================================================

// On-disk key: 16 bytes, no player bit, no padding.
#pragma pack(push, 1)
typedef struct _BoardKeyDisk
{
    uint64_t ullCellsInUse;
    uint64_t ullCellColors;
} BOARD_KEY_DISK, *PBOARD_KEY_DISK;
#pragma pack(pop)
static_assert(sizeof(BOARD_KEY_DISK) == 16, "BOARD_KEY_DISK must be 16 bytes");

#define BLF_MAGIC 0x424C535446494C45ULL   // "BLSTFILE" in little-endian ASCII

#pragma pack(push, 1)
typedef struct __BlasterFileTrailer
{
    uint8_t  minKey[16];     // first BOARD_KEY_DISK in sorted order
    uint8_t  maxKey[16];     // last  BOARD_KEY_DISK in sorted order
    uint64_t recordCount;    // BOARD_KEY_DISK records preceding this trailer
    uint8_t  _reserved[16]; // reserved, must be zero
    uint64_t magic;          // BLF_MAGIC — written last; absence = incomplete file
} BlasterFileTrailer, *PBlasterFileTrailer;
#pragma pack(pop)
static_assert(sizeof(BlasterFileTrailer) == 64, "BlasterFileTrailer must be 64 bytes");

// Write count sorted BOARD_KEY_DISK records to path, followed by the trailer.
// Records must already be sorted and deduped.  Fatals on any I/O error.
void BLFWrite(const char* path, const BOARD_KEY_DISK* pKeys, uint64_t count);

// Streaming writer — records are fed one at a time (e.g. from a k-way merge).
// BLFWriterOpen  : plain 16-byte records, CRT write buffer = BLF_WRITE_BUFFER_SIZE.
// BLFWriterOpenZ : delta+varint compressed output (.blfz), own I/O buffer.
// BLFWriterRecord and BLFWriterClose work identically for both writers.
// BLFWriterClose writes the trailer, closes the file, and returns the record count.
#define BLF_WRITE_BUFFER_SIZE      (512  * 1024)
#define BLF_COMP_WRITE_BUFFER_SIZE (64   * 1024)
#define BLF_COMP_READ_BUFFER_SIZE  (1024 * 1024)

// Second magic value: delta+varint compressed store file.
// _reserved[0..7] in the trailer stores the compressed byte count (uint64_t LE).
#define BLFZ_MAGIC 0x5A46494C54534C42ULL   // "BLSTFILZ" in little-endian ASCII

typedef struct __BLFWriter BLFWriter;

BLFWriter* BLFWriterOpen(const char* path);
BLFWriter* BLFWriterOpenZ(const char* path);   // compressed variant
void       BLFWriterRecord(BLFWriter* pw, const BOARD_KEY_DISK* pKey);
// Returns the record count.  If pFileBytes is non-null, also stores the actual
// number of bytes written to the file (compressed or uncompressed payload + trailer).
uint64_t   BLFWriterClose(BLFWriter* pw, uint64_t* pFileBytes = nullptr);

// Opaque sequential reader.
typedef struct __BLFReader BLFReader;

// Open a blaster file for sequential reading.
// Validates the trailer (magic + size sanity) before returning.
// Returns nullptr if the file is missing, incomplete, or corrupt — does NOT fatal.
BLFReader* BLFOpen(const char* path);

// Read up to maxCount BOARD_KEY_DISK records into pOut.
// Returns count actually read; 0 = EOF.
int BLFRead(BLFReader* r, BOARD_KEY_DISK* pOut, int maxCount);

// Trailer from an open reader (valid until BLFClose).
const BlasterFileTrailer* BLFTrailer(const BLFReader* r);

void BLFClose(BLFReader** ppReader);
