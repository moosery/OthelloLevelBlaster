#pragma once
#include <OthelloBasics.h>

// Board-size constants that device functions need in lieu of the host globals
// (g_boardMask, g_boardRightEdge, g_boardLeftEdge).
// Populate with OBCuda_GetBoardConsts() after calling SetBoardSizeForRun().
struct DevBoardConsts {
    unsigned long long boardMask;
    unsigned long long boardRightEdge;
    unsigned long long boardLeftEdge;
};

// Host function: captures current g_board* globals into a DevBoardConsts.
DevBoardConsts OBCuda_GetBoardConsts();

// ─────────────────────────────────────────────────────────────────────────────
// Device functions — available only in CUDA compilation units (.cu files).
// All mirror the corresponding OthelloBasics CPU functions exactly.
// numRotations passed to dev_canonicalize must be 1, 4, or 8 (not 16 —
// BoardFlip is not implemented here).
// ─────────────────────────────────────────────────────────────────────────────
#ifdef __CUDACC__

// Manual byte-swap (replaces _byteswap_uint64 which is an MSVC-only intrinsic).
__device__ __forceinline__
unsigned long long dev_bswap64(unsigned long long x)
{
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) <<  8) |
           ((x & 0x000000FF00000000ULL) >>  8) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

// Transpose an 8x8 bit matrix stored in a uint64 along the A1-H8 diagonal.
__device__ __forceinline__
unsigned long long dev_flipDiagA1H8(unsigned long long x)
{
    unsigned long long t;
    const unsigned long long k1 = 0x5500550055005500ULL;
    const unsigned long long k2 = 0x3333000033330000ULL;
    const unsigned long long k4 = 0x0F0F0F0F00000000ULL;
    t  = k4 & (x ^ (x << 28)); x ^= t ^ (t >> 28);
    t  = k2 & (x ^ (x << 14)); x ^= t ^ (t >> 14);
    t  = k1 & (x ^ (x <<  7)); x ^= t ^ (t >>  7);
    return x;
}

// 90-degree clockwise rotation: byteswap then diagonal transpose.
__device__ __forceinline__
void dev_rotate90Right(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullPossibleMoves = 0;
    dst->ullCellsInUse    = dev_flipDiagA1H8(dev_bswap64(src->ullCellsInUse));
    dst->ullCellColors    = dev_flipDiagA1H8(dev_bswap64(src->ullCellColors));
}

// Reverse bits within each byte independently (mirrors column order per row).
__device__ __forceinline__
unsigned long long dev_mirrorBytewise(unsigned long long x)
{
    x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((x & 0x5555555555555555ULL) << 1);
    return x;
}

// Mirror the board around the vertical axis.
__device__ __forceinline__
void dev_mirrorVerticalAxis(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullCellsInUse    = dev_mirrorBytewise(src->ullCellsInUse);
    dst->ullCellColors    = dev_mirrorBytewise(src->ullCellColors);
    dst->ullPossibleMoves = 0;
}

// Returns true if board a sorts before board b (mirrors BoardCompare < 0).
// Comparison order: ullCellsInUse, ullCellColors, next-player (Black='B'=66 < White='W'=87).
__device__ __forceinline__
bool dev_boardLT(const BOARD* a, const BOARD* b)
{
    if (a->ullCellsInUse != b->ullCellsInUse)
        return a->ullCellsInUse < b->ullCellsInUse;
    if (a->ullCellColors != b->ullCellColors)
        return a->ullCellColors < b->ullCellColors;
    // bit=1 → Black='B'=66; bit=0 → White='W'=87.  Black sorts first → higher bit = less.
    return (a->usBoardInfo & 0x01u) > (b->usBoardInfo & 0x01u);
}

// Compute all pieces flipped by placing moveBit for player against opponent.
// Uses full-grid 8x8 column masks — correct for all board sizes.
__device__ __forceinline__
unsigned long long dev_computeFlips(unsigned long long moveBit,
                                    unsigned long long player,
                                    unsigned long long opponent)
{
    const unsigned long long NLC = ~0x8080808080808080ULL; // not left  col
    const unsigned long long NRC = ~0x0101010101010101ULL; // not right col
    unsigned long long flips = 0, x;

    x  = (moveBit << 8) & opponent;
    x |= (x      << 8) & opponent; x |= (x << 8) & opponent;
    x |= (x      << 8) & opponent; x |= (x << 8) & opponent;
    x |= (x      << 8) & opponent;
    if ((x << 8) & player) flips |= x;

    x  = (moveBit >> 8) & opponent;
    x |= (x      >> 8) & opponent; x |= (x >> 8) & opponent;
    x |= (x      >> 8) & opponent; x |= (x >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    if ((x >> 8) & player) flips |= x;

    x  = ((moveBit & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent; x |= ((x & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent; x |= ((x & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent;
    if  ((x & NRC) >> 1  & player) flips |= x;

    x  = ((moveBit & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent; x |= ((x & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent; x |= ((x & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent;
    if  ((x & NLC) << 1  & player) flips |= x;

    x  = ((moveBit & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent; x |= ((x & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent; x |= ((x & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent;
    if  ((x & NRC) >> 9  & player) flips |= x;

    x  = ((moveBit & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent; x |= ((x & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent; x |= ((x & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent;
    if  ((x & NLC) >> 7  & player) flips |= x;

    x  = ((moveBit & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent; x |= ((x & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent; x |= ((x & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent;
    if  ((x & NRC) << 7  & player) flips |= x;

    x  = ((moveBit & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent; x |= ((x & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent; x |= ((x & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent;
    if  ((x & NLC) << 9  & player) flips |= x;

    return flips;
}

// Apply a move at moveIdx (0=top-left, 63=bottom-right) for color onto board.
__device__ __forceinline__
void dev_applyMove(BOARD* board, char color, int moveIdx)
{
    unsigned long long moveBit  = FIRSTBIT >> moveIdx;
    unsigned long long occupied = board->ullCellsInUse;
    unsigned long long colors   = board->ullCellColors;

    unsigned long long player, opponent;
    if (color == BLACK) {
        player   = occupied &  colors;
        opponent = occupied & ~colors;
    } else {
        player   = occupied & ~colors;
        opponent = occupied &  colors;
    }

    unsigned long long flips = dev_computeFlips(moveBit, player, opponent);

    board->ullCellsInUse |= moveBit;
    if (color == BLACK)
        board->ullCellColors |= (moveBit | flips);
    else
        board->ullCellColors &= ~(moveBit | flips);
}

// Produce the result board after playing moveIdx on src; result written to dst.
// Mirrors MovePlayAndSetResultBoard.
__device__ __forceinline__
void dev_playMove(const BOARD* src, BOARD* dst, int moveIdx)
{
    dst->ullCellsInUse    = src->ullCellsInUse;
    dst->ullCellColors    = src->ullCellColors;
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullPossibleMoves = 0;
    dst->ullBlackWins     = 0;
    dst->ullWhiteWins     = 0;
    dst->ullTies          = 0;
    dst->usBoardState     = 0;

    char color = GETBOARDNEXTPLAYER(src);
    SETBOARDNEXTPLAYERFLIP(dst);
    dev_applyMove(dst, color, moveIdx);
}

// Compute ullPossibleMoves for the current player using 8-direction bitboard fill.
__device__ __forceinline__
void dev_boardMoveCalculator(BOARD* board, const DevBoardConsts& c)
{
    char color = GETBOARDNEXTPLAYER(board);

    unsigned long long myPieces, oppPieces;
    if (color == BLACK) {
        myPieces  = board->ullCellsInUse &  board->ullCellColors;
        oppPieces = board->ullCellsInUse & ~board->ullCellColors;
    } else {
        myPieces  = board->ullCellsInUse & ~board->ullCellColors;
        oppPieces = board->ullCellsInUse &  board->ullCellColors;
    }

    const unsigned long long notRight = ~c.boardRightEdge;
    const unsigned long long notLeft  = ~c.boardLeftEdge;
    unsigned long long empty      = c.boardMask & ~(myPieces | oppPieces);
    unsigned long long validMoves = 0;
    unsigned long long gen, candidates;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 1; gen &= candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates;
    validMoves |= ((gen & notRight) >> 1) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 1; gen &= candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates;
    validMoves |= ((gen & notLeft) << 1) & empty;

    candidates = oppPieces;
    gen  = (myPieces >> 8) & candidates;
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    validMoves |= (gen >> 8) & empty;

    candidates = oppPieces;
    gen  = (myPieces << 8) & candidates;
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    validMoves |= (gen << 8) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 9; gen &= candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates;
    validMoves |= ((gen & notRight) >> 9) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) >> 7; gen &= candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates;
    validMoves |= ((gen & notLeft) >> 7) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) << 7; gen &= candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates;
    validMoves |= ((gen & notRight) << 7) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 9; gen &= candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates;
    validMoves |= ((gen & notLeft) << 9) & empty;

    board->ullPossibleMoves = validMoves;
}

// Compute legal moves for the current player directly from a BOARD_KEY.
// Mirrors dev_boardMoveCalculator but takes a key and returns the move mask.
__device__ __forceinline__
unsigned long long dev_boardKeyGetMoves(const BOARD_KEY* key, const DevBoardConsts& c)
{
    char color = GETBOARDNEXTPLAYER(key);

    unsigned long long myPieces, oppPieces;
    if (color == BLACK) {
        myPieces  = key->ullCellsInUse &  key->ullCellColors;
        oppPieces = key->ullCellsInUse & ~key->ullCellColors;
    } else {
        myPieces  = key->ullCellsInUse & ~key->ullCellColors;
        oppPieces = key->ullCellsInUse &  key->ullCellColors;
    }

    const unsigned long long notRight = ~c.boardRightEdge;
    const unsigned long long notLeft  = ~c.boardLeftEdge;
    unsigned long long empty      = c.boardMask & ~(myPieces | oppPieces);
    unsigned long long validMoves = 0;
    unsigned long long gen, candidates;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 1; gen &= candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates;
    validMoves |= ((gen & notRight) >> 1) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 1; gen &= candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates;
    validMoves |= ((gen & notLeft) << 1) & empty;

    candidates = oppPieces;
    gen  = (myPieces >> 8) & candidates;
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    validMoves |= (gen >> 8) & empty;

    candidates = oppPieces;
    gen  = (myPieces << 8) & candidates;
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    validMoves |= (gen << 8) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 9; gen &= candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates;
    validMoves |= ((gen & notRight) >> 9) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) >> 7; gen &= candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates;
    validMoves |= ((gen & notLeft) >> 7) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) << 7; gen &= candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates;
    validMoves |= ((gen & notRight) << 7) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 9; gen &= candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates;
    validMoves |= ((gen & notLeft) << 9) & empty;

    return validMoves;
}

// ── BOARD_KEY device functions ────────────────────────────────────────────────
// Parallel set to the BOARD-based functions above.  OLE uses only these;
// SolverKernel.cu continues to use the BOARD-based versions.

__device__ __forceinline__
void dev_applyMove_key(BOARD_KEY* board, char color, int moveIdx)
{
    unsigned long long moveBit  = FIRSTBIT >> moveIdx;
    unsigned long long occupied = board->ullCellsInUse;
    unsigned long long colors   = board->ullCellColors;

    unsigned long long player, opponent;
    if (color == BLACK) {
        player   = occupied &  colors;
        opponent = occupied & ~colors;
    } else {
        player   = occupied & ~colors;
        opponent = occupied &  colors;
    }

    unsigned long long flips = dev_computeFlips(moveBit, player, opponent);

    board->ullCellsInUse |= moveBit;
    if (color == BLACK)
        board->ullCellColors |= (moveBit | flips);
    else
        board->ullCellColors &= ~(moveBit | flips);
}

__device__ __forceinline__
void dev_rotate90Right_key(const BOARD_KEY* src, BOARD_KEY* dst)
{
    dst->usBoardInfo   = src->usBoardInfo;
    dst->ullCellsInUse = dev_flipDiagA1H8(dev_bswap64(src->ullCellsInUse));
    dst->ullCellColors = dev_flipDiagA1H8(dev_bswap64(src->ullCellColors));
}

__device__ __forceinline__
void dev_mirrorVerticalAxis_key(const BOARD_KEY* src, BOARD_KEY* dst)
{
    dst->usBoardInfo   = src->usBoardInfo;
    dst->ullCellsInUse = dev_mirrorBytewise(src->ullCellsInUse);
    dst->ullCellColors = dev_mirrorBytewise(src->ullCellColors);
}

__device__ __forceinline__
void dev_boardFlip_key(const BOARD_KEY* src, BOARD_KEY* dst)
{
    dst->usBoardInfo   = src->usBoardInfo ^ 0x01u;
    dst->ullCellsInUse = src->ullCellsInUse;
    dst->ullCellColors = ~src->ullCellColors & src->ullCellsInUse;
}

__device__ __forceinline__
bool dev_boardLT_key(const BOARD_KEY* a, const BOARD_KEY* b)
{
    if (a->ullCellsInUse != b->ullCellsInUse)
        return a->ullCellsInUse < b->ullCellsInUse;
    if (a->ullCellColors != b->ullCellColors)
        return a->ullCellColors < b->ullCellColors;
    return (a->usBoardInfo & 0x01u) > (b->usBoardInfo & 0x01u);
}

__device__ __forceinline__
void dev_playMove_key(const BOARD_KEY* src, BOARD_KEY* dst, int moveIdx)
{
    dst->ullCellsInUse = src->ullCellsInUse;
    dst->ullCellColors = src->ullCellColors;
    dst->usBoardInfo   = src->usBoardInfo;
    char color = GETBOARDNEXTPLAYER(src);
    SETBOARDNEXTPLAYERFLIP(dst);
    dev_applyMove_key(dst, color, moveIdx);
}

// Canonicalize a BOARD_KEY in-place: try up to numRotations symmetries, keep
// the minimum under key ordering.  No moves computed — caller calls
// dev_boardKeyGetMoves if needed.  numRotations: 1, 4, 8, or 16.
// _pad1 bytes stay zero because arr is zero-initialized and rotation functions
// only write the three named fields.
__device__ __forceinline__
void dev_canonicalize_key(BOARD_KEY* board, int numRotations)
{
    BOARD_KEY arr[16] = {};

    arr[0].ullCellsInUse = board->ullCellsInUse;
    arr[0].ullCellColors = board->ullCellColors;
    arr[0].usBoardInfo   = board->usBoardInfo;

    if (numRotations >= 4) {
        dev_rotate90Right_key(&arr[0], &arr[1]);
        dev_rotate90Right_key(&arr[1], &arr[2]);
        dev_rotate90Right_key(&arr[2], &arr[3]);
    }
    if (numRotations >= 8) {
        dev_mirrorVerticalAxis_key(&arr[0], &arr[4]);
        dev_rotate90Right_key(&arr[4], &arr[5]);
        dev_rotate90Right_key(&arr[5], &arr[6]);
        dev_rotate90Right_key(&arr[6], &arr[7]);
    }
    if (numRotations >= 16) {
        dev_boardFlip_key(&arr[0], &arr[8]);
        dev_rotate90Right_key(&arr[8],  &arr[9]);
        dev_rotate90Right_key(&arr[9],  &arr[10]);
        dev_rotate90Right_key(&arr[10], &arr[11]);
        dev_mirrorVerticalAxis_key(&arr[8], &arr[12]);
        dev_rotate90Right_key(&arr[12], &arr[13]);
        dev_rotate90Right_key(&arr[13], &arr[14]);
        dev_rotate90Right_key(&arr[14], &arr[15]);
    }

    int n = (numRotations >= 16) ? 16
          : (numRotations >=  8) ?  8
          : (numRotations >=  4) ?  4 : 1;
    int minIdx = 0;
    for (int i = 1; i < n; i++) {
        if (dev_boardLT_key(&arr[i], &arr[minIdx]))
            minIdx = i;
    }

    board->ullCellsInUse = arr[minIdx].ullCellsInUse;
    board->ullCellColors = arr[minIdx].ullCellColors;
    board->usBoardInfo   = arr[minIdx].usBoardInfo;
}
// ─────────────────────────────────────────────────────────────────────────────

// Swap Black↔White by complementing ullCellColors within occupied cells and
// flipping the next-player bit.  Used to generate the color-mirror symmetry
// for 16-rotation canonicalization.
__device__ __forceinline__
void dev_boardFlip(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo ^ 0x01u;           // flip current player
    dst->ullCellsInUse    = src->ullCellsInUse;
    dst->ullCellColors    = ~src->ullCellColors & src->ullCellsInUse; // swap Black↔White
    dst->ullPossibleMoves = 0;
}

// Canonicalize board in-place: generate up to numRotations symmetries, keep the
// minimum under BoardCompare ordering, then compute ullPossibleMoves on the winner.
// numRotations: 1, 4, 8, or 16.  16 includes the color-swap (BoardFlip) symmetry.
__device__ __forceinline__
void dev_canonicalize(BOARD* board, int numRotations, const DevBoardConsts& c)
{
    BOARD arr[16];

    arr[0].ullCellsInUse    = board->ullCellsInUse;
    arr[0].ullCellColors    = board->ullCellColors;
    arr[0].usBoardInfo      = board->usBoardInfo;
    arr[0].ullPossibleMoves = 0;

    if (numRotations >= 4) {
        dev_rotate90Right(&arr[0], &arr[1]);
        dev_rotate90Right(&arr[1], &arr[2]);
        dev_rotate90Right(&arr[2], &arr[3]);
    }
    if (numRotations >= 8) {
        dev_mirrorVerticalAxis(&arr[0], &arr[4]);
        dev_rotate90Right(&arr[4], &arr[5]);
        dev_rotate90Right(&arr[5], &arr[6]);
        dev_rotate90Right(&arr[6], &arr[7]);
    }
    if (numRotations >= 16) {
        dev_boardFlip(&arr[0], &arr[8]);
        dev_rotate90Right(&arr[8],  &arr[9]);
        dev_rotate90Right(&arr[9],  &arr[10]);
        dev_rotate90Right(&arr[10], &arr[11]);
        dev_mirrorVerticalAxis(&arr[8], &arr[12]);
        dev_rotate90Right(&arr[12], &arr[13]);
        dev_rotate90Right(&arr[13], &arr[14]);
        dev_rotate90Right(&arr[14], &arr[15]);
    }

    int n = (numRotations >= 16) ? 16
          : (numRotations >=  8) ?  8
          : (numRotations >=  4) ?  4 : 1;
    int minIdx = 0;
    for (int i = 1; i < n; i++) {
        if (dev_boardLT(&arr[i], &arr[minIdx]))
            minIdx = i;
    }

    board->ullCellsInUse    = arr[minIdx].ullCellsInUse;
    board->ullCellColors    = arr[minIdx].ullCellColors;
    board->usBoardInfo      = arr[minIdx].usBoardInfo;
    board->ullPossibleMoves = 0;

    dev_boardMoveCalculator(board, c);
}

#endif // __CUDACC__
