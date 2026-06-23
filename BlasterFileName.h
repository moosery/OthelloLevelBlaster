#pragma once
#include <stdio.h>

// ============================================================================
// File naming convention for all BLF files produced by OthelloLevelBlaster.
//
// Every file is player-specific (black or white turn) and uses .blf extension.
//
// Store files  (storeDir):         Level_0017_6x6_black_0000.blf
//                                  Level_0017_6x6_white_0000.blf
// Writer files (mwDirectory[i]):   writer_black_0042.blf
//                                  writer_white_0042.blf
// Imerge files (mergeDirectory[i]):imerge_L017_black_0042.blf
//                                  imerge_L017_white_0042.blf
// Cascade temp (tempDir):          cascade_temp_L017_black_0042.blf
//                                  cascade_temp_L017_white_0042.blf
// ============================================================================

#define BLF_PLAYER_BLACK 1
#define BLF_PLAYER_WHITE 0

static inline const char* BLFPlayerStr(int player)
{
    return player ? "black" : "white";
}

// Returns BLF_PLAYER_BLACK or BLF_PLAYER_WHITE by scanning the filename for
// the literal strings "black" or "white".  Returns -1 if neither is found.
static inline int BLFPlayerFromPath(const char* path)
{
    // Walk backwards from the end so we match the player token in the filename
    // portion, not a directory component that happens to contain the word.
    const char* p = path + strlen(path);
    while (p > path && *p != '\\' && *p != '/') --p;
    if (strstr(p, "black")) return BLF_PLAYER_BLACK;
    if (strstr(p, "white")) return BLF_PLAYER_WHITE;
    return -1;
}

// ── Store files ──────────────────────────────────────────────────────────────

static inline void BLFNameStoreFile(char* out, size_t outSize,
                                     const char* dir, int boardSize,
                                     int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.blf",
             dir, level, boardSize, boardSize, BLFPlayerStr(player), fileIdx);
}

// Wildcard pattern matching all store files for a given level and player.
static inline void BLFPatternStoreFiles(char* out, size_t outSize,
                                         const char* dir, int boardSize,
                                         int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_*.blf",
             dir, level, boardSize, boardSize, BLFPlayerStr(player));
}

// Wildcard pattern matching ALL store files for a given level (both players).
static inline void BLFPatternAnyStoreFiles(char* out, size_t outSize,
                                            const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_*.blf",
             dir, level, boardSize, boardSize);
}

// ── Writer files (NVMe MW buffers) ──────────────────────────────────────────

static inline void BLFNameWriterFile(char* out, size_t outSize,
                                      const char* dir, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\writer_%s_%04d.blf",
             dir, BLFPlayerStr(player), fileIdx);
}

static inline void BLFPatternWriterFiles(char* out, size_t outSize,
                                          const char* dir, int player)
{
    snprintf(out, outSize, "%s\\writer_%s_*.blf", dir, BLFPlayerStr(player));
}

// ── Intermediate merge files (medium drives) ─────────────────────────────────

static inline void BLFNameImergeFile(char* out, size_t outSize,
                                      const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_%04d.blf",
             dir, level, BLFPlayerStr(player), fileIdx);
}

static inline void BLFPatternImergeFiles(char* out, size_t outSize,
                                          const char* dir, int level, int player)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_*.blf",
             dir, level, BLFPlayerStr(player));
}

// ── Compressed store files (.blfz) ───────────────────────────────────────────
// Same naming as store files but with .blfz extension.
// Used when --compress is active; BLFOpen detects format from the magic value.

static inline void BLFZNameStoreFile(char* out, size_t outSize,
                                      const char* dir, int boardSize,
                                      int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.blfz",
             dir, level, boardSize, boardSize, BLFPlayerStr(player), fileIdx);
}

static inline void BLFZPatternStoreFiles(char* out, size_t outSize,
                                          const char* dir, int boardSize,
                                          int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_*.blfz",
             dir, level, boardSize, boardSize, BLFPlayerStr(player));
}

static inline void BLFZPatternAnyStoreFiles(char* out, size_t outSize,
                                             const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_*.blfz",
             dir, level, boardSize, boardSize);
}

// ── Cascade temp files ───────────────────────────────────────────────────────

static inline void BLFNameCascadeTemp(char* out, size_t outSize,
                                       const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\cascade_temp_L%03d_%s_%04d.blf",
             dir, level, BLFPlayerStr(player), fileIdx);
}

// ── Compressed writer files (.blfz) ─────────────────────────────────────────

static inline void BLFZNameWriterFile(char* out, size_t outSize,
                                       const char* dir, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\writer_%s_%04d.blfz",
             dir, BLFPlayerStr(player), fileIdx);
}

static inline void BLFZPatternWriterFiles(char* out, size_t outSize,
                                           const char* dir, int player)
{
    snprintf(out, outSize, "%s\\writer_%s_*.blfz", dir, BLFPlayerStr(player));
}

// ── Compressed intermediate merge files (.blfz) ──────────────────────────────

static inline void BLFZNameImergeFile(char* out, size_t outSize,
                                       const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_%04d.blfz",
             dir, level, BLFPlayerStr(player), fileIdx);
}

static inline void BLFZPatternImergeFiles(char* out, size_t outSize,
                                           const char* dir, int level, int player)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_*.blfz",
             dir, level, BLFPlayerStr(player));
}

// ── Compressed cascade temp files (.blfz) ────────────────────────────────────

static inline void BLFZNameCascadeTemp(char* out, size_t outSize,
                                        const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\cascade_temp_L%03d_%s_%04d.blfz",
             dir, level, BLFPlayerStr(player), fileIdx);
}

// ── Level sentinel files (storeDir) ─────────────────────────────────────────
// Zero-byte markers written to storeDir to track merge state.
//   Level_NNNN_merging  — created at the START of DoEndOfLevelMerge
//   Level_NNNN_complete — created when BOTH player threads finish cleanly;
//                         merging sentinel is deleted at the same time.
// The resume scan uses these to distinguish a complete level (has _complete)
// from an interrupted one (has _merging but no _complete).
// Old data produced before sentinels were added must have sentinel files
// created manually: type nul > "Y:\...\storeDir\Level_NNNN_complete"

static inline void SentinelNameMerging(char* out, size_t outSize,
                                        const char* dir, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_merging", dir, level);
}

static inline void SentinelNameComplete(char* out, size_t outSize,
                                         const char* dir, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_complete", dir, level);
}

// Magic value embedded at the start of a _complete sentinel that contains serialized
// LevelStats.  Zero-byte sentinel files (legacy / manually created) have no magic
// and are treated as "level complete, stats unknown."
#define SENTINEL_STATS_MAGIC 0x5354415453544C42ULL  // "BLTSTATS" LE
