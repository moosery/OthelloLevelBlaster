#include "BlasterFile.h"
#include "Error.h"
#include "Logger.h"
#include "Mem.h"
#include <string.h>
#include <stdio.h>

struct __BLFWriter
{
    FILE*          f;
    uint64_t       count;
    BOARD_KEY_DISK firstKey;
    BOARD_KEY_DISK lastKey;
    bool           hasFirst;
};

BLFWriter* BLFWriterOpen(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "BLFWriterOpen: cannot create '%s'", path);
    setvbuf(f, NULL, _IOFBF, BLF_WRITE_BUFFER_SIZE);

    BLFWriter* pw = (BLFWriter*)MemMalloc("BLFWriter", sizeof(BLFWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "BLFWriterOpen: cannot allocate writer"); }
    pw->f        = f;
    pw->count    = 0;
    pw->hasFirst = false;
    return pw;
}

void BLFWriterRecord(BLFWriter* pw, const BOARD_KEY_DISK* pKey)
{
    if (fwrite(pKey, sizeof(BOARD_KEY_DISK), 1, pw->f) != 1)
        Fatal(FATAL_FILE_OPEN, "BLFWriterRecord: write failed");
    if (!pw->hasFirst) { pw->firstKey = *pKey; pw->hasFirst = true; }
    pw->lastKey = *pKey;
    pw->count++;
}

uint64_t BLFWriterClose(BLFWriter* pw)
{
    BlasterFileTrailer trailer = {};
    trailer.recordCount = pw->count;
    if (pw->hasFirst)
    {
        memcpy(trailer.minKey, &pw->firstKey, sizeof(BOARD_KEY_DISK));
        memcpy(trailer.maxKey, &pw->lastKey,  sizeof(BOARD_KEY_DISK));
    }
    trailer.magic = BLF_MAGIC;

    if (fwrite(&trailer, sizeof(trailer), 1, pw->f) != 1)
        Fatal(FATAL_FILE_OPEN, "BLFWriterClose: trailer write failed");
    fclose(pw->f);

    uint64_t count = pw->count;
    MemFree(pw);
    return count;
}

struct __BLFReader
{
    FILE*              f;
    BlasterFileTrailer trailer;
    uint64_t           recordsRead;
};

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
    if (fread(&trailer, sizeof(trailer), 1, f) != 1 || trailer.magic != BLF_MAGIC)
    {
        fclose(f);
        LoggerLog("BLFOpen: bad or missing trailer in '%s' (corrupt or incomplete)\n", path);
        return nullptr;
    }

    int64_t expectedSize = (int64_t)trailer.recordCount * (int64_t)sizeof(BOARD_KEY_DISK)
                         + (int64_t)sizeof(BlasterFileTrailer);
    _fseeki64(f, 0, SEEK_END);
    int64_t actualSize = _ftelli64(f);
    if (actualSize != expectedSize)
    {
        fclose(f);
        LoggerLog("BLFOpen: size mismatch in '%s' (expected %lld, got %lld)\n",
                  path, expectedSize, actualSize);
        return nullptr;
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

    r->f           = f;
    r->trailer     = trailer;
    r->recordsRead = 0;
    return r;
}

int BLFRead(BLFReader* r, BOARD_KEY_DISK* pOut, int maxCount)
{
    uint64_t remaining = r->trailer.recordCount - r->recordsRead;
    if (remaining == 0 || maxCount <= 0) return 0;

    int want = (remaining < (uint64_t)maxCount) ? (int)remaining : maxCount;
    int got  = (int)fread(pOut, sizeof(BOARD_KEY_DISK), (size_t)want, r->f);
    r->recordsRead += (uint64_t)got;
    return got;
}

const BlasterFileTrailer* BLFTrailer(const BLFReader* r)
{
    return &r->trailer;
}

void BLFClose(BLFReader** ppReader)
{
    if (!ppReader || !*ppReader) return;
    if ((*ppReader)->f) fclose((*ppReader)->f);
    MemFree(*ppReader);
    *ppReader = nullptr;
}
