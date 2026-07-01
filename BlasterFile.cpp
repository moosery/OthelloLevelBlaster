#include "BlasterFile.h"
#include "Error.h"
#include "Logger.h"
#include "Mem.h"
#include "FileAndDirUtils.h"
#include "lz4frame.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <windows.h>

// ============================================================
// Delta + zigzag helpers (used by both writer and reader)
// ============================================================
static inline uint64_t ZZEnc(int64_t v) { return (v < 0) ? (uint64_t)(-v * 2 - 1) : (uint64_t)(v * 2); }
static inline int64_t  ZZDec(uint64_t v) { return (v & 1) ? -(int64_t)((v >> 1) + 1) : (int64_t)(v >> 1); }

static inline size_t VarIntPut(uint64_t v, uint8_t* out)
{
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

// ============================================================
// BLFWriter
// ============================================================
struct __BLFWriter
{
    FILE*          f;
    char           path[MAX_FULL_PATH_NAME];
    uint64_t       count;
    BOARD_KEY_DISK firstKey;
    BOARD_KEY_DISK lastKey;
    bool           hasFirst;
    bool           compressed;
    // compressed-only
    uint8_t*       varBuf;
    size_t         varBufPos;
    uint64_t       compBytesTotal;
    uint64_t       prevF0;
    uint64_t       prevF1;
    // LZ4 layer (.blfzl) — only when lz4Cctx != nullptr
    LZ4F_cctx*     lz4Cctx;
    uint8_t*        lz4OutBuf;
    size_t          lz4OutBufSize;
};

static void FlushVarBuf(BLFWriter* pw)
{
    if (pw->varBufPos == 0) return;

    if (pw->lz4Cctx)
    {
        size_t compSize = LZ4F_compressUpdate(pw->lz4Cctx,
                                               pw->lz4OutBuf, pw->lz4OutBufSize,
                                               pw->varBuf,    pw->varBufPos, nullptr);
        if (LZ4F_isError(compSize))
            Fatal(FATAL_FILE_OPEN,
                  "FlushVarBuf: LZ4 compress failed on '%s': %s",
                  pw->path, LZ4F_getErrorName(compSize));
        if (compSize > 0)
        {
            if (fwrite(pw->lz4OutBuf, 1, compSize, pw->f) != compSize)
                Fatal(FATAL_FILE_OPEN,
                      "FlushVarBuf: LZ4 write failed on '%s'", pw->path);
            pw->compBytesTotal += compSize;
        }
    }
    else
    {
        if (fwrite(pw->varBuf, 1, pw->varBufPos, pw->f) != pw->varBufPos)
        {
            DWORD err = GetLastError();
            Fatal(FATAL_FILE_OPEN,
                  "BLFWriterRecord: compressed write failed on '%s' "
                  "(tried %zu bytes, GetLastError=%lu, errno=%d)",
                  pw->path, pw->varBufPos, err, errno);
        }
        pw->compBytesTotal += pw->varBufPos;
    }
    pw->varBufPos = 0;
}

BLFWriter* BLFWriterOpen(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "BLFWriterOpen: cannot create '%s'", path);
    setvbuf(f, NULL, _IOFBF, BLF_WRITE_BUFFER_SIZE);

    BLFWriter* pw = (BLFWriter*)MemMalloc("BLFWriter", sizeof(BLFWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "BLFWriterOpen: cannot allocate writer"); }
    memset(pw, 0, sizeof(BLFWriter));
    pw->f = f;
    strncpy(pw->path, path, sizeof(pw->path) - 1);
    return pw;
}

BLFWriter* BLFWriterOpenZ(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "BLFWriterOpenZ: cannot create '%s'", path);
    setvbuf(f, NULL, _IOFBF, 64 * 1024 * 1024);

    BLFWriter* pw = (BLFWriter*)MemMalloc("BLFWriterZ", sizeof(BLFWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "BLFWriterOpenZ: cannot allocate writer"); }
    memset(pw, 0, sizeof(BLFWriter));
    pw->f          = f;
    pw->compressed = true;
    strncpy(pw->path, path, sizeof(pw->path) - 1);

    pw->varBuf = (uint8_t*)MemMalloc("BLFWriterZBuf", BLF_COMP_WRITE_BUFFER_SIZE);
    if (!pw->varBuf)
    {
        fclose(f); MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED, "BLFWriterOpenZ: cannot allocate write buffer");
    }

    // .blfzl: add LZ4 frame layer on top of varint
    if (strstr(path, ".blfzl"))
    {
        LZ4F_errorCode_t lz4Err = LZ4F_createCompressionContext(&pw->lz4Cctx, LZ4F_VERSION);
        if (LZ4F_isError(lz4Err))
        {
            fclose(f); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_ALLOCATION_FAILED,
                  "BLFWriterOpenZ: LZ4 context create failed on '%s': %s",
                  path, LZ4F_getErrorName(lz4Err));
        }

        pw->lz4OutBufSize = LZ4F_compressBound(BLF_COMP_WRITE_BUFFER_SIZE, nullptr)
                          + LZ4F_HEADER_SIZE_MAX + 32;
        pw->lz4OutBuf = (uint8_t*)MemMalloc("BLFWriterZLBuf", pw->lz4OutBufSize);
        if (!pw->lz4OutBuf)
        {
            LZ4F_freeCompressionContext(pw->lz4Cctx);
            fclose(f); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_ALLOCATION_FAILED,
                  "BLFWriterOpenZ: cannot allocate LZ4 output buffer");
        }

        // Write LZ4 frame header; enable content checksum for integrity verification
        LZ4F_preferences_t prefs      = {};
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        size_t headerSize = LZ4F_compressBegin(pw->lz4Cctx,
                                                pw->lz4OutBuf, pw->lz4OutBufSize, &prefs);
        if (LZ4F_isError(headerSize))
        {
            LZ4F_freeCompressionContext(pw->lz4Cctx);
            fclose(f); MemFree(pw->lz4OutBuf); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_FILE_OPEN,
                  "BLFWriterOpenZ: LZ4 frame begin failed on '%s': %s",
                  path, LZ4F_getErrorName(headerSize));
        }
        if (fwrite(pw->lz4OutBuf, 1, headerSize, pw->f) != headerSize)
        {
            LZ4F_freeCompressionContext(pw->lz4Cctx);
            fclose(f); MemFree(pw->lz4OutBuf); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_FILE_OPEN,
                  "BLFWriterOpenZ: LZ4 header write failed on '%s'", path);
        }
        pw->compBytesTotal += headerSize;
    }

    return pw;
}

void BLFWriterRecord(BLFWriter* pw, const BOARD_KEY_DISK* pKey)
{
    if (!pw->compressed)
    {
        if (fwrite(pKey, sizeof(BOARD_KEY_DISK), 1, pw->f) != 1)
            Fatal(FATAL_FILE_OPEN, "BLFWriterRecord: write failed");
    }
    else
    {
        // Ensure room for two max-length varints (10 bytes each)
        if (pw->varBufPos + 20 > BLF_COMP_WRITE_BUFFER_SIZE)
            FlushVarBuf(pw);
        uint64_t df0 = ZZEnc((int64_t)pKey->ullCellsInUse - (int64_t)pw->prevF0);
        uint64_t df1 = ZZEnc((int64_t)pKey->ullCellColors  - (int64_t)pw->prevF1);
        pw->varBufPos += VarIntPut(df0, pw->varBuf + pw->varBufPos);
        pw->varBufPos += VarIntPut(df1, pw->varBuf + pw->varBufPos);
        pw->prevF0 = pKey->ullCellsInUse;
        pw->prevF1 = pKey->ullCellColors;
    }

    if (!pw->hasFirst) { pw->firstKey = *pKey; pw->hasFirst = true; }
    pw->lastKey = *pKey;
    pw->count++;
}

uint64_t BLFWriterClose(BLFWriter* pw, uint64_t* pFileBytes)
{
    if (pw->compressed)
        FlushVarBuf(pw);

    // End the LZ4 frame (writes end mark + content checksum)
    if (pw->lz4Cctx)
    {
        size_t endSize = LZ4F_compressEnd(pw->lz4Cctx,
                                           pw->lz4OutBuf, pw->lz4OutBufSize, nullptr);
        if (LZ4F_isError(endSize))
            Fatal(FATAL_FILE_OPEN,
                  "BLFWriterClose: LZ4 frame end failed on '%s': %s",
                  pw->path, LZ4F_getErrorName(endSize));
        if (endSize > 0)
        {
            if (fwrite(pw->lz4OutBuf, 1, endSize, pw->f) != endSize)
                Fatal(FATAL_FILE_OPEN,
                      "BLFWriterClose: LZ4 end write failed on '%s'", pw->path);
            pw->compBytesTotal += endSize;
        }
        LZ4F_freeCompressionContext(pw->lz4Cctx);
        MemFree(pw->lz4OutBuf);
    }

    BlasterFileTrailer trailer = {};
    trailer.recordCount = pw->count;
    if (pw->hasFirst)
    {
        memcpy(trailer.minKey, &pw->firstKey, sizeof(BOARD_KEY_DISK));
        memcpy(trailer.maxKey, &pw->lastKey,  sizeof(BOARD_KEY_DISK));
    }

    uint64_t fileBytes;
    if (pw->compressed)
    {
        memcpy(trailer._reserved, &pw->compBytesTotal, sizeof(uint64_t));
        trailer.magic = strstr(pw->path, ".blfzl") ? BLFZL_MAGIC : BLFZ_MAGIC;
        fileBytes     = pw->compBytesTotal + sizeof(BlasterFileTrailer);
        MemFree(pw->varBuf);
    }
    else
    {
        trailer.magic = BLF_MAGIC;
        fileBytes     = pw->count * sizeof(BOARD_KEY_DISK) + sizeof(BlasterFileTrailer);
    }

    if (fwrite(&trailer, sizeof(trailer), 1, pw->f) != 1)
        Fatal(FATAL_FILE_OPEN, "BLFWriterClose: trailer write failed");
    fclose(pw->f);

    uint64_t count = pw->count;
    MemFree(pw);
    if (pFileBytes) *pFileBytes = fileBytes;
    return count;
}

// ============================================================
// BLFWrite (batch, always uncompressed)
// ============================================================
void BLFWrite(const char* path, const BOARD_KEY_DISK* pKeys, uint64_t count)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "BLFWrite: cannot create '%s'", path);

    if (count > 0 && fwrite(pKeys, sizeof(BOARD_KEY_DISK), (size_t)count, f) != (size_t)count)
    {
        fclose(f);
        Fatal(FATAL_FILE_OPEN, "BLFWrite: record write failed for '%s'", path);
    }

    BlasterFileTrailer trailer = {};
    trailer.recordCount = count;
    if (count > 0)
    {
        memcpy(trailer.minKey, &pKeys[0],         sizeof(BOARD_KEY_DISK));
        memcpy(trailer.maxKey, &pKeys[count - 1], sizeof(BOARD_KEY_DISK));
    }
    trailer.magic = BLF_MAGIC;

    if (fwrite(&trailer, sizeof(trailer), 1, f) != 1)
    {
        fclose(f);
        Fatal(FATAL_FILE_OPEN, "BLFWrite: trailer write failed for '%s'", path);
    }

    fclose(f);
}

// ============================================================
// BLFReader (handles .blf, .blfz, and .blfzl via magic dispatch)
// ============================================================
struct __BLFReader
{
    FILE*              f;
    BlasterFileTrailer trailer;
    uint64_t           recordsRead;
    bool               compressed;
    // compressed-only (.blfz and .blfzl)
    uint8_t*           compBuf;
    size_t             compBufSize;
    size_t             compBufPos;
    size_t             compBufFilled;
    uint64_t           compBytesTotal;
    uint64_t           compBytesConsumed;
    uint64_t           prevF0;
    uint64_t           prevF1;
    // LZ4 decompression layer (.blfzl only) — nullptr for .blfz
    LZ4F_dctx*         lz4Dctx;
    uint8_t*            lz4DecBuf;       // decompressed varint bytes
    size_t              lz4DecBufSize;
    size_t              lz4DecBufPos;    // read position in lz4DecBuf
    size_t              lz4DecBufFilled; // valid bytes in lz4DecBuf
    bool                lz4FrameDone;    // true after LZ4F_decompress returned 0
};

static uint8_t BLFZReadByte(BLFReader* r)
{
    if (r->lz4Dctx)
    {
        // LZ4 path: serve bytes from the decompressed varint buffer.
        // Refill by decompressing from compBuf (which holds raw LZ4 frame data).
        while (r->lz4DecBufPos >= r->lz4DecBufFilled)
        {
            if (r->lz4FrameDone)
                Fatal(FATAL_FILE_OPEN,
                      "BLFZReadByte: read past end of LZ4 frame in '%s'",
                      r->trailer.magic == BLFZL_MAGIC ? "(blfzl)" : "?");

            // Refill compBuf from disk if empty
            if (r->compBufPos >= r->compBufFilled)
            {
                uint64_t remaining = r->compBytesTotal - r->compBytesConsumed;
                if (remaining == 0)
                    Fatal(FATAL_FILE_OPEN,
                          "BLFZReadByte: LZ4 compressed stream exhausted before frame end");
                size_t toRead = (remaining < (uint64_t)r->compBufSize)
                                ? (size_t)remaining : r->compBufSize;
                r->compBufFilled = fread(r->compBuf, 1, toRead, r->f);
                r->compBufPos    = 0;
                if (r->compBufFilled == 0)
                    Fatal(FATAL_FILE_OPEN, "BLFZReadByte: LZ4 read failed");
            }

            size_t srcSize = r->compBufFilled - r->compBufPos;
            size_t dstSize = r->lz4DecBufSize;
            size_t ret = LZ4F_decompress(r->lz4Dctx,
                                          r->lz4DecBuf, &dstSize,
                                          r->compBuf + r->compBufPos, &srcSize,
                                          nullptr);
            if (LZ4F_isError(ret))
                Fatal(FATAL_FILE_OPEN,
                      "BLFZReadByte: LZ4 decompress error: %s",
                      LZ4F_getErrorName(ret));
            r->compBufPos        += srcSize;
            r->compBytesConsumed += srcSize;
            r->lz4DecBufPos       = 0;
            r->lz4DecBufFilled    = dstSize;
            if (ret == 0)
                r->lz4FrameDone = true;
            // dstSize may be 0 when consuming LZ4 overhead (end mark, checksum);
            // loop again to get the next decompressed chunk.
        }
        return r->lz4DecBuf[r->lz4DecBufPos++];
    }

    // Original varint-only path (.blfz)
    if (r->compBufPos >= r->compBufFilled)
    {
        uint64_t remaining = r->compBytesTotal - r->compBytesConsumed;
        size_t toRead = (remaining < (uint64_t)r->compBufSize) ? (size_t)remaining : r->compBufSize;
        r->compBufFilled = fread(r->compBuf, 1, toRead, r->f);
        r->compBufPos    = 0;
    }
    r->compBytesConsumed++;
    return r->compBuf[r->compBufPos++];
}

static uint64_t BLFZReadVarInt(BLFReader* r)
{
    uint64_t v = 0; int sh = 0;
    for (;;) {
        uint8_t b = BLFZReadByte(r);
        v |= (uint64_t)(b & 0x7F) << sh;
        sh += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}

BLFReader* BLFOpen(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    if (_fseeki64(f, -(int64_t)sizeof(BlasterFileTrailer), SEEK_END) != 0)
    {
        fclose(f);
        LoggerLog("BLFOpen: cannot seek to trailer in '%s'\n", path);
        return nullptr;
    }

    BlasterFileTrailer trailer = {};
    if (fread(&trailer, sizeof(trailer), 1, f) != 1)
    {
        fclose(f);
        LoggerLog("BLFOpen: cannot read trailer in '%s'\n", path);
        return nullptr;
    }

    bool compressed = false;
    bool lz4        = false;
    if (trailer.magic == BLF_MAGIC)
        compressed = false;
    else if (trailer.magic == BLFZ_MAGIC)
        compressed = true;
    else if (trailer.magic == BLFZL_MAGIC)
        { compressed = true; lz4 = true; }
    else
    {
        fclose(f);
        LoggerLog("BLFOpen: bad magic in '%s' (corrupt or incomplete)\n", path);
        return nullptr;
    }

    _fseeki64(f, 0, SEEK_END);
    int64_t actualSize = _ftelli64(f);

    if (!compressed)
    {
        int64_t expectedSize = (int64_t)trailer.recordCount * (int64_t)sizeof(BOARD_KEY_DISK)
                             + (int64_t)sizeof(BlasterFileTrailer);
        if (actualSize != expectedSize)
        {
            fclose(f);
            LoggerLog("BLFOpen: size mismatch in '%s' (expected %lld, got %lld)\n",
                      path, expectedSize, actualSize);
            return nullptr;
        }
    }
    else
    {
        uint64_t compressedBytes = 0;
        memcpy(&compressedBytes, trailer._reserved, sizeof(uint64_t));
        int64_t expectedSize = (int64_t)compressedBytes + (int64_t)sizeof(BlasterFileTrailer);
        if (actualSize != expectedSize)
        {
            fclose(f);
            LoggerLog("BLFOpen: compressed size mismatch in '%s' (expected %lld, got %lld)\n",
                      path, expectedSize, actualSize);
            return nullptr;
        }
    }

    if (_fseeki64(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return nullptr;
    }

    BLFReader* r = (BLFReader*)MemMalloc("BLFReader", sizeof(BLFReader));
    if (!r)
    {
        fclose(f);
        Fatal(FATAL_ALLOCATION_FAILED, "BLFOpen: cannot allocate reader");
        return nullptr;
    }
    memset(r, 0, sizeof(BLFReader));
    r->f          = f;
    r->trailer    = trailer;
    r->compressed = compressed;

    if (!compressed)
        setvbuf(f, NULL, _IOFBF, BLF_COMP_READ_BUFFER_SIZE);

    if (compressed)
    {
        uint64_t compressedBytes = 0;
        memcpy(&compressedBytes, trailer._reserved, sizeof(uint64_t));
        r->compBuf        = (uint8_t*)MemMalloc("BLFReaderZBuf", BLF_COMP_READ_BUFFER_SIZE);
        r->compBufSize    = BLF_COMP_READ_BUFFER_SIZE;
        r->compBytesTotal = compressedBytes;
        if (!r->compBuf)
        {
            fclose(f); MemFree(r);
            Fatal(FATAL_ALLOCATION_FAILED, "BLFOpen: cannot allocate read buffer");
            return nullptr;
        }

        if (lz4)
        {
            LZ4F_errorCode_t lz4Err =
                LZ4F_createDecompressionContext(&r->lz4Dctx, LZ4F_VERSION);
            if (LZ4F_isError(lz4Err))
            {
                fclose(f); MemFree(r->compBuf); MemFree(r);
                Fatal(FATAL_ALLOCATION_FAILED,
                      "BLFOpen: LZ4 decomp context failed: %s",
                      LZ4F_getErrorName(lz4Err));
                return nullptr;
            }
            r->lz4DecBufSize = BLF_COMP_READ_BUFFER_SIZE;
            r->lz4DecBuf = (uint8_t*)MemMalloc("BLFReaderZLBuf", r->lz4DecBufSize);
            if (!r->lz4DecBuf)
            {
                LZ4F_freeDecompressionContext(r->lz4Dctx);
                fclose(f); MemFree(r->compBuf); MemFree(r);
                Fatal(FATAL_ALLOCATION_FAILED,
                      "BLFOpen: cannot allocate LZ4 decomp buffer");
                return nullptr;
            }
        }
    }

    return r;
}

int BLFRead(BLFReader* r, BOARD_KEY_DISK* pOut, int maxCount)
{
    uint64_t remaining = r->trailer.recordCount - r->recordsRead;
    if (remaining == 0 || maxCount <= 0) return 0;
    int want = (remaining < (uint64_t)maxCount) ? (int)remaining : maxCount;

    if (!r->compressed)
    {
        int got = (int)fread(pOut, sizeof(BOARD_KEY_DISK), (size_t)want, r->f);
        r->recordsRead += (uint64_t)got;
        return got;
    }

    int got = 0;
    while (got < want)
    {
        uint64_t df0 = BLFZReadVarInt(r);
        uint64_t df1 = BLFZReadVarInt(r);
        r->prevF0 = (uint64_t)((int64_t)r->prevF0 + ZZDec(df0));
        r->prevF1 = (uint64_t)((int64_t)r->prevF1 + ZZDec(df1));
        pOut[got].ullCellsInUse = r->prevF0;
        pOut[got].ullCellColors = r->prevF1;
        got++;
        r->recordsRead++;
    }
    return got;
}

const BlasterFileTrailer* BLFTrailer(const BLFReader* r)
{
    return &r->trailer;
}

void BLFClose(BLFReader** ppReader)
{
    if (!ppReader || !*ppReader) return;
    BLFReader* r = *ppReader;
    if (r->f) fclose(r->f);
    if (r->lz4Dctx)   LZ4F_freeDecompressionContext(r->lz4Dctx);
    if (r->lz4DecBuf)  MemFree(r->lz4DecBuf);
    if (r->compressed && r->compBuf) MemFree(r->compBuf);
    MemFree(r);
    *ppReader = nullptr;
}
