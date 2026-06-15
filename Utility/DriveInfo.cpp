#include "DriveInfo.h"
#include "FileAndDirUtils.h"
#include "Error.h"
#include "Logger.h"
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>

// ============================================================================
// Cache  (single JSON file: driveinfo.json)
// Detection fields (free space, drive type, etc.) are always re-queried fresh;
// only benchmark fields are loaded from cache.
// ============================================================================

static const int CACHE_JSON_VERSION = 1;

// Remote drives below this write speed get only the quick single-pass probe;
// faster remote drives (e.g. Samba shares) get the full multi-dir benchmark.
static constexpr double NAS_MULTDIR_THRESHOLD_MBS = 50.0;

static void BuildCacheFilePath(const char* cacheDir, char* out, size_t outSz)
{
    snprintf(out, outSz, "%s\\driveinfo.json", cacheDir);
}

// ---- minimal JSON helpers (flat objects only) ----

static bool JsStr(const char* obj, const char* key, char* out, size_t outSz)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i < outSz - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool JsDbl(const char* obj, const char* key, double* out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char* end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool JsInt(const char* obj, const char* key, int* out)
{
    double v;
    if (!JsDbl(obj, key, &v)) return false;
    *out = (int)v;
    return true;
}

static bool JsBool(const char* obj, const char* key, bool* out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

// Returns a malloc'd copy of the JSON object containing "letter": "X",
// or nullptr if not found.  Caller must free().
static char* ExtractDriveBlock(const char* json, char letter)
{
    char target[32];
    snprintf(target, sizeof(target), "\"letter\": \"%c\"", (char)toupper((unsigned char)letter));

    const char* found = strstr(json, target);
    if (!found) return nullptr;

    const char* start = found;
    while (start > json && *start != '{') --start;
    if (*start != '{') return nullptr;

    const char* end = found;
    while (*end && *end != '}') ++end;
    if (!*end) return nullptr;
    ++end;

    size_t len = (size_t)(end - start);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

// Loads benchmark fields from JSON text for the given drive letter.
// Returns true only if a matching valid entry is found and the serial matches.
static bool LoadCacheJSON(const char* jsonText, char letter, const char* expectedSerial,
                          DriveInformation* pOut)
{
    char* block = ExtractDriveBlock(jsonText, letter);
    if (!block) return false;

    bool ok = false;
    do {
        char serial[64] = {};
        JsStr(block, "serial", serial, sizeof(serial));
        if (expectedSerial && expectedSerial[0] && serial[0] &&
            strcmp(serial, expectedSerial) != 0)
            break;

        bool valid = false;
        if (!JsBool(block, "benchmarkValid", &valid) || !valid) break;

        int    optimalDirs      = 0;
        double writeMBs         = 0, readMBs = 0;
        double combinedWriteMBs = 0, combinedReadMBs = 0;
        char   timestamp[32]    = {};

        if (!JsInt(block, "optimalDirs",      &optimalDirs))      break;
        if (!JsDbl(block, "writeMBs",         &writeMBs))         break;
        if (!JsDbl(block, "readMBs",          &readMBs))          break;
        if (!JsDbl(block, "combinedWriteMBs", &combinedWriteMBs)) break;
        if (!JsDbl(block, "combinedReadMBs",  &combinedReadMBs))  break;
        JsStr(block, "timestamp", timestamp, sizeof(timestamp));

        pOut->benchmarkValid   = true;
        pOut->optimalDirs      = optimalDirs;
        pOut->writeMBs         = writeMBs;
        pOut->readMBs          = readMBs;
        pOut->combinedWriteMBs = combinedWriteMBs;
        pOut->combinedReadMBs  = combinedReadMBs;
        memcpy(pOut->timestamp, timestamp, sizeof(pOut->timestamp));
        ok = true;
    } while (false);

    free(block);
    return ok;
}

// Writes all drives with valid benchmark data to the JSON cache file.
static void SaveCacheJSON(const char* path, const PMachineDriveInfo pMDI)
{
    FILE* f = fopen(path, "w");
    if (!f)
        Fatal(FATAL_DRIVE_CACHE_WRITE_FAILED,
              "DriveInfo: cannot write cache file '%s'", path);

    fprintf(f, "{\n  \"version\": %d,\n  \"drives\": [\n", CACHE_JSON_VERSION);

    bool first = true;
    for (int i = 0; i < pMDI->numDrives; i++) {
        const DriveInformation* p = &pMDI->drives[i];
        if (!p->benchmarkValid) continue;
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f,
            "    {\n"
            "      \"letter\": \"%c\",\n"
            "      \"serial\": \"%s\",\n"
            "      \"benchmarkValid\": true,\n"
            "      \"optimalDirs\": %d,\n"
            "      \"writeMBs\": %.2f,\n"
            "      \"readMBs\": %.2f,\n"
            "      \"combinedWriteMBs\": %.2f,\n"
            "      \"combinedReadMBs\": %.2f,\n"
            "      \"timestamp\": \"%s\"\n"
            "    }",
            (char)toupper((unsigned char)p->driveLetter),
            p->serial,
            p->optimalDirs,
            p->writeMBs,
            p->readMBs,
            p->combinedWriteMBs,
            p->combinedReadMBs,
            p->timestamp);
    }

    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

// ============================================================================
// Drive detection
// ============================================================================

#ifndef BusTypeNvme
#define BusTypeNvme ((STORAGE_BUS_TYPE)0x11)
#endif

static HANDLE OpenDriveHandle(const char* path)
{
    return CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

static bool QuerySeekPenalty(HANDLE hPhys, bool& outRotational)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType  = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR spd = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), &spd, sizeof(spd), &returned, nullptr))
        return false;
    outRotational = (spd.IncursSeekPenalty == TRUE);
    return true;
}

static bool QueryDeviceProps(HANDLE hPhys, STORAGE_BUS_TYPE& outBusType,
                              char* serial, size_t serialSz)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    char buf[1024] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &returned, nullptr))
        return false;
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    outBusType = desc->BusType;
    if (serial && serialSz > 0) {
        serial[0] = '\0';
        if (desc->SerialNumberOffset != 0 && desc->SerialNumberOffset < returned) {
            strncpy_s(serial, serialSz, buf + desc->SerialNumberOffset, _TRUNCATE);
            size_t len = strlen(serial);
            while (len > 0 && serial[len - 1] == ' ') serial[--len] = '\0';
        }
    }
    return true;
}

static int QueryDiskExtents(HANDLE hVolume, DWORD diskNums[], int maxDiskNums)
{
    char buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, buf, sizeof(buf), &returned, nullptr))
        return 0;
    auto* vde = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
    int count = (int)vde->NumberOfDiskExtents;
    if (count > maxDiskNums) count = maxDiskNums;
    for (int i = 0; i < count; i++)
        diskNums[i] = vde->Extents[i].DiskNumber;
    return count;
}

static void QueryOneDrive(char letter, DriveInformation* pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    pOut->driveLetter    = letter;
    pOut->primaryDiskNum = -1;

    char rootPath[4] = { letter, ':', '\\', '\0' };

    pOut->isNas = (GetDriveTypeA(rootPath) == DRIVE_REMOTE);

    ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFree = {};
    if (GetDiskFreeSpaceExA(rootPath, &freeBytesAvail, &totalBytes, &totalFree)) {
        pOut->totalBytes  = totalBytes.QuadPart;
        pOut->freeBytes   = freeBytesAvail.QuadPart;
        pOut->usableBytes = (pOut->freeBytes > DRIVE_SAFETY_MARGIN_BYTES)
                          ? pOut->freeBytes - DRIVE_SAFETY_MARGIN_BYTES : 0;
    }

    // NAS/network drives don't support IOCTL queries — skip them.
    if (pOut->isNas) {
        pOut->available = (pOut->totalBytes > 0);
        return;
    }

    char volPath[7] = { '\\','\\','.','\\', letter, ':', '\0' };
    HANDLE hVol = OpenDriveHandle(volPath);
    if (hVol == INVALID_HANDLE_VALUE) {
        pOut->available = (pOut->totalBytes > 0);
        return;
    }

    DWORD diskNums[16] = {};
    int numExtents = QueryDiskExtents(hVol, diskNums, 16);
    CloseHandle(hVol);

    if (numExtents < 1) {
        pOut->available = false;
        return;
    }
    pOut->primaryDiskNum = (int)diskNums[0];
    pOut->numSpindles    = numExtents;

    char physPath[32];
    snprintf(physPath, sizeof(physPath), "\\\\.\\PhysicalDrive%d", pOut->primaryDiskNum);
    HANDLE hPhys = OpenDriveHandle(physPath);
    if (hPhys == INVALID_HANDLE_VALUE) {
        pOut->available = false;
        return;
    }

    bool rotational = false;
    QuerySeekPenalty(hPhys, rotational);
    pOut->isRotational = rotational;

    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    QueryDeviceProps(hPhys, busType, pOut->serial, sizeof(pOut->serial));
    pOut->isNvme = (busType == BusTypeNvme);

    CloseHandle(hPhys);
    pOut->available = true;
}

// ============================================================================
// Benchmark
// ============================================================================

static const size_t BENCH_CHUNK = 4ULL * 1024 * 1024;   // 4 MB I/O chunk

static double BenchNowSecs()
{
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static void* BenchAlloc(size_t bytes)
{
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void BenchFree(void* p)
{
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

static double BenchWritePass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;
    double t0 = BenchNowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk = (DWORD)std::min(BENCH_CHUNK, rem);
        DWORD written = 0;
        if (!WriteFile(h, buf, chunk, &written, nullptr) || written != chunk) break;
        rem -= written;
    }
    double elapsed = BenchNowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0 * 1024.0 * elapsed) : 0.0;
}

static double BenchReadPass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;
    double t0 = BenchNowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk = (DWORD)std::min(BENCH_CHUNK, rem);
        DWORD bytesRead = 0;
        if (!ReadFile(h, buf, chunk, &bytesRead, nullptr) || bytesRead != chunk) break;
        rem -= chunk;
    }
    double elapsed = BenchNowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0 * 1024.0 * elapsed) : 0.0;
}

static double BenchMedian(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) * 0.5 : v[n / 2];
}

static bool RunConcurrent(
    char letter, int numDirs, size_t fileBytes, int numPasses, bool verbose,
    double& outPerDirWrite, double& outPerDirRead,
    double& outCombinedWrite, double& outCombinedRead)
{
    std::vector<std::string> paths(numDirs);
    for (int d = 0; d < numDirs; d++) {
        char buf[MAX_PATH];
        snprintf(buf, sizeof(buf), "%c:\\drv_bench_tmp_%03d.dat", letter, d + 1);
        paths[d] = buf;
    }

    std::vector<void*> bufs(numDirs, nullptr);
    for (int d = 0; d < numDirs; d++) {
        bufs[d] = BenchAlloc(BENCH_CHUNK);
        if (!bufs[d]) {
            for (int j = 0; j < d; j++) BenchFree(bufs[j]);
            return false;
        }
        memset(bufs[d], 0xA5, BENCH_CHUNK);
    }

    std::vector<std::vector<double>> writeResults(numDirs), readResults(numDirs);

    for (int pass = 0; pass < numPasses; pass++) {
        std::vector<double> passWrite(numDirs, 0.0);
        std::vector<std::thread> threads;
        std::atomic<int> barrier{ 0 };

        for (int d = 0; d < numDirs; d++) {
            threads.emplace_back([&, d]() {
                barrier.fetch_add(1);
                while (barrier.load() < numDirs) {}
                passWrite[d] = BenchWritePass(paths[d].c_str(), bufs[d], fileBytes);
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();

        std::vector<double> passRead(numDirs, 0.0);
        barrier.store(0);
        for (int d = 0; d < numDirs; d++) {
            threads.emplace_back([&, d]() {
                barrier.fetch_add(1);
                while (barrier.load() < numDirs) {}
                passRead[d] = BenchReadPass(paths[d].c_str(), bufs[d], fileBytes);
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();

        if (pass == 0) {
            if (verbose) LoggerLog("      pass 1 (warmup) discarded\n");
            continue;
        }

        for (int d = 0; d < numDirs; d++) {
            writeResults[d].push_back(passWrite[d]);
            readResults[d].push_back(passRead[d]);
        }

        if (verbose) {
            double sumW = 0, sumR = 0;
            for (int d = 0; d < numDirs; d++) { sumW += passWrite[d]; sumR += passRead[d]; }
            LoggerLog("      pass %d: write %.0f MB/s  read %.0f MB/s  (combined)\n",
                   pass + 1, sumW, sumR);
        }
    }

    for (int d = 0; d < numDirs; d++) {
        DeleteFileA(paths[d].c_str());
        BenchFree(bufs[d]);
    }

    double sumW = 0.0, sumR = 0.0;
    for (int d = 0; d < numDirs; d++) {
        sumW += BenchMedian(writeResults[d]);
        sumR += BenchMedian(readResults[d]);
    }
    outCombinedWrite = sumW;
    outCombinedRead  = sumR;
    outPerDirWrite   = sumW / numDirs;
    outPerDirRead    = sumR / numDirs;
    return true;
}

static void BenchmarkOneDrive(
    char letter,
    size_t fileBytes, int numPasses, double threshold, int maxDirs, bool verbose,
    DriveInformation* pOut)
{
    for (int i = 1; i <= maxDirs; i++) {
        char stale[MAX_PATH];
        snprintf(stale, sizeof(stale), "%c:\\drv_bench_tmp_%03d.dat", letter, i);
        DeleteFileA(stale);
    }

    double prevCombW = 0.0, prevCombR = 0.0;
    int    bestDirs  = 1;
    double bestPerDirW = 0.0, bestPerDirR = 0.0;
    double bestCombW   = 0.0, bestCombR   = 0.0;

    for (int numDirs = 1; numDirs <= maxDirs; numDirs++) {
        if (verbose)
            LoggerLog("    Testing %d dir%s on %c: (%zu MB x %d passes, pass 1 discarded)...\n",
                   numDirs, numDirs > 1 ? "s" : "", letter,
                   fileBytes / (1024 * 1024), numPasses);

        double perDirW, perDirR, combW, combR;
        if (!RunConcurrent(letter, numDirs, fileBytes, numPasses, verbose,
                           perDirW, perDirR, combW, combR)) {
            if (verbose)
                LoggerLog("    [benchmark failed for %d dir(s) on %c:]\n", numDirs, letter);
            break;
        }

        double writeGain = (prevCombW > 0.0) ? (combW - prevCombW) / prevCombW : 0.0;
        double readGain  = (prevCombR > 0.0) ? (combR - prevCombR) / prevCombR : 0.0;
        double avgGain   = (writeGain + readGain) / 2.0;

        if (verbose) {
            if (numDirs == 1) {
                LoggerLog("      Result: write %.0f MB/s  read %.0f MB/s\n", combW, combR);
            } else {
                LoggerLog("      Result: combined write %.0f MB/s  read %.0f MB/s"
                       "  (write +%.0f%%  read +%.0f%%  vs %d dir)\n",
                       combW, combR, writeGain * 100.0, readGain * 100.0, numDirs - 1);
                if (avgGain < threshold)
                    LoggerLog("      avg %.0f%% < %.0f%% threshold -- stopping at %d dir%s\n",
                           avgGain * 100.0, threshold * 100.0, bestDirs, bestDirs > 1 ? "s" : "");
                else
                    LoggerLog("      avg %.0f%% >= %.0f%% threshold -- keeping %d dirs\n",
                           avgGain * 100.0, threshold * 100.0, numDirs);
            }
        }

        bool improved = (numDirs == 1) || (avgGain >= threshold);
        if (improved) {
            bestDirs  = numDirs;
            bestPerDirW = perDirW;  bestPerDirR = perDirR;
            bestCombW   = combW;    bestCombR   = combR;
        }

        prevCombW = combW;
        prevCombR = combR;

        if (!improved && numDirs > 1) break;
    }

    pOut->benchmarkValid   = true;
    pOut->optimalDirs      = bestDirs;
    pOut->writeMBs         = bestPerDirW;
    pOut->readMBs          = bestPerDirR;
    pOut->combinedWriteMBs = bestCombW;
    pOut->combinedReadMBs  = bestCombR;
}

// ============================================================================
// RefreshDriveFreeSpace
// ============================================================================

void RefreshDriveFreeSpace(PMachineDriveInfo pMDI)
{
    for (int i = 0; i < pMDI->numDrives; i++) {
        DriveInformation* p = &pMDI->drives[i];
        if (!p->available) continue;
        char rootPath[4] = { p->driveLetter, ':', '\\', '\0' };
        ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFree = {};
        if (GetDiskFreeSpaceExA(rootPath, &freeBytesAvail, &totalBytes, &totalFree)) {
            p->totalBytes  = totalBytes.QuadPart;
            p->freeBytes   = freeBytesAvail.QuadPart;
            p->usableBytes = (p->freeBytes > DRIVE_SAFETY_MARGIN_BYTES)
                           ? p->freeBytes - DRIVE_SAFETY_MARGIN_BYTES : 0;
        }
    }
}

// ============================================================================
// GetDriveInformation
// ============================================================================

void GetDriveInformation(
    PMachineDriveInfo pMachineDriveInfo,
    const char*       pCacheDir,
    const char*       driveLetters,
    bool              forceBenchmark)
{
    std::string letters;
    if (driveLetters && *driveLetters) {
        letters = driveLetters;
    } else {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(mask & (1u << i))) continue;
            char root[4] = { (char)('A' + i), ':', '\\', '\0' };
            if (GetDriveTypeA(root) == DRIVE_FIXED)
                letters += (char)('A' + i);
        }
    }

    // Load the JSON cache file once up front.
    char cachePath[MAX_PATH] = {};
    char* cacheText = nullptr;
    if (pCacheDir && pCacheDir[0]) {
        BuildCacheFilePath(pCacheDir, cachePath, sizeof(cachePath));
        FILE* fc = fopen(cachePath, "r");
        if (fc) {
            fseek(fc, 0, SEEK_END);
            long sz = ftell(fc);
            fseek(fc, 0, SEEK_SET);
            if (sz > 0) {
                cacheText = (char*)malloc((size_t)sz + 1);
                if (cacheText) {
                    fread(cacheText, 1, (size_t)sz, fc);
                    cacheText[sz] = '\0';
                }
            }
            fclose(fc);
        }
    }

    bool anyBenchmarked = false;
    int count = 0;
    for (char ch : letters) {
        if (count >= MAX_SYSTEM_DRIVES) break;
        DriveInformation& info = pMachineDriveInfo->drives[count];

        QueryOneDrive(ch, &info);

        if (!info.available) {
            count++;
            continue;
        }

        bool cacheLoaded = false;

        if (cacheText && !forceBenchmark)
            cacheLoaded = LoadCacheJSON(cacheText, ch, info.serial, &info);

        if (!cacheLoaded) {
            if (info.isNas) {
                BenchmarkOneDrive(ch, 256ULL * 1024 * 1024, 2, 0.10, 1, true, &info);
                if (info.writeMBs >= NAS_MULTDIR_THRESHOLD_MBS) {
                    LoggerLog("    %c: %.0f MB/s >= %.0f MB/s threshold -- re-benchmarking with multi-dir\n",
                           ch, info.writeMBs, NAS_MULTDIR_THRESHOLD_MBS);
                    BenchmarkOneDrive(ch, 256ULL * 1024 * 1024, 3, 0.10, 4, true, &info);
                }
            } else {
                BenchmarkOneDrive(ch, 256ULL * 1024 * 1024, 5, 0.10, 4, true, &info);
            }

            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(info.timestamp, sizeof(info.timestamp),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

            anyBenchmarked = true;
        }

        if (info.writeMBs >= FAST_DRIVE_MB_THRESHOLD)
            info.driveCategory = DRIVE_CAT_FAST;
        else if (info.writeMBs >= MEDIUM_DRIVE_MB_THRESHOLD)
            info.driveCategory = DRIVE_CAT_MEDIUM;
        else
            info.driveCategory = DRIVE_CAT_SLOW;

        count++;
    }

    pMachineDriveInfo->numDrives = count;

    if (anyBenchmarked && pCacheDir && pCacheDir[0]) {
        if (!CreateFullPath(pCacheDir))
            Fatal(FATAL_DRIVE_CACHE_WRITE_FAILED,
                  "DriveInfo: cannot create cache directory '%s'", pCacheDir);
        SaveCacheJSON(cachePath, pMachineDriveInfo);
    }

    free(cacheText);
}

// ============================================================================
// PrintDriveInformation
// ============================================================================

void PrintDriveInformation(const PMachineDriveInfo pMachineDriveInfo)
{
    LoggerLog("Detected %d drive(s):\n", pMachineDriveInfo->numDrives);

    for (int i = 0; i < pMachineDriveInfo->numDrives; i++) {
        const DriveInformation* p = &pMachineDriveInfo->drives[i];
        if (!p->available) {
            LoggerLog("  %c:  [unavailable]\n", p->driveLetter);
            continue;
        }

        const char* typeStr = p->isNas        ? "NAS"
                            : p->isNvme       ? "NVMe"
                            : p->isRotational ? "HDD"
                            :                   "SSD";
        double totalTB  = (double)p->totalBytes  / (1024.0 * 1024 * 1024 * 1024);
        double freeTB   = (double)p->freeBytes   / (1024.0 * 1024 * 1024 * 1024);
        double usableTB = (double)p->usableBytes / (1024.0 * 1024 * 1024 * 1024);

        LoggerLog("  %c:  %-4s  disk#%d  spindles=%d"
               "  total=%.2f TB  free=%.2f TB  usable=%.2f TB\n",
               p->driveLetter, typeStr, p->primaryDiskNum, p->numSpindles,
               totalTB, freeTB, usableTB);

        if (p->benchmarkValid)
            LoggerLog("       bench: %d dir%s optimal"
                   "  Write: %.0f MB/s/dir (%.0f combined)  Read: %.0f MB/s/dir"
                   "  [%s]\n",
                   p->optimalDirs, p->optimalDirs > 1 ? "s" : "",
                   p->writeMBs, p->combinedWriteMBs, p->readMBs, p->timestamp);
        else
            LoggerLog("       bench: not available\n");
    }
}
