#pragma once
#include "Error.h"
#include <stdio.h>

/* 24 byte for a board key */
typedef struct _BoardKey
{
	unsigned long long  ullCellsInUse;      /* 0-> Not used      1-> Used             */
	unsigned long long  ullCellColors;      /* 0-> White         1-> Black            */
	unsigned short      usBoardInfo;        /* 0b0000000X        Next Player 1->Black */
	                                        /*                               0->White */
	unsigned short      _pad1[3];           /* explicit alignment padding             */
} BOARD_KEY, * PBOARD_KEY;
static_assert(sizeof(BOARD_KEY) == 24, "BOARD_KEY must be 24 bytes");

/* 64 bytes for a board */
typedef struct _Board
{
	unsigned long long  ullCellsInUse;      /* 0-> Not used      1-> Used             */
	unsigned long long  ullCellColors;      /* 0-> White         1-> Black            */
	unsigned short      usBoardInfo;        /* 0b0000000X        Next Player 1->Black */
	                                        /*                               0->White */
	unsigned short      _pad1[3];           /* explicit alignment padding             */
	unsigned long long  ullPossibleMoves;   /* 0-> No move       1-> Can play         */
	                                        /* If all 0xFFFFFFFFFFFFFFFF then no move */
	                                        /* for this player but other player can   */
	                                        /* make a move                            */
	unsigned long long  ullBlackWins;       /* Number of potential black wins         */
	unsigned long long  ullWhiteWins;       /* Number of potential white wins         */
	unsigned long long  ullTies;            /* Number of tie boards                   */
	unsigned short      usBoardState;       /* The state of the board                 */
	                                        /* 0   -> Not Played                      */
											/* 1   -> Played but not a terminal board */
	                                        /* 2   -> Played and is a terminal board  */
											/* 3   -> Played but no moves avail       */
											/*        So find the next board with     */
											/*        player flipped!                 */
	unsigned short      _pad2[3];           /* explicit trailing padding              */
} BOARD, * PBOARD;

/* 48 bytes for a move */
typedef struct _Move
{
	unsigned long long  ullCellsInUseParent;/* 0-> Not used      1-> Used             */
	unsigned long long  ullCellColorsParent;/* 0-> White         1-> Black            */
	unsigned short      usBoardInfoParent;  /* 0b0000000X        Next Player 1->Black */
											/*                               0->White */
	unsigned short      usMoveIdx;          /* 100 -> Just a change in players         */
	                                        /* Otherwise 0->63 starting in upper left */
	                                        /* Moving to lower right in row first ord */
	unsigned int        _pad1;              /* explicit alignment padding             */
	unsigned long long  ullCellsInUseResult;/* 0-> Not used      1-> Used             */
	unsigned long long  ullCellColorsResult;/* 0-> White         1-> Black            */
	unsigned short      usBoardInfoResult;  /* 0b0000000X        Next Player 1->Black */
											/*                               0->White */
	unsigned short      _pad2[3];           /* explicit trailing padding              */
} MOVE, * PMOVE;

/* The bit we move all around */
#define FIRSTBIT						   ((unsigned long long) 0x8000000000000000)

/* BIT Index Macro */
#define GETINDEX(row,col)                  ((row * 8) + col)
#define GETROWFROMINDEX(idx)               ((idx) / 8)
#define GETCOLFROMINDEX(idx)               ((idx) % 8)

/* Occupied Macros */
#define GETNUMINUSE(pBoard)                (int) (__popcnt64((pBoard)->ullCellsInUse))
#define GETNUMEMPTY(pBoard)                (64-GETNUMINUSE((pBoard)))
#define SETOCCUPIEDINDEX(pBoard,idx)       (pBoard)->ullCellsInUse = (pBoard)->ullCellsInUse | (FIRSTBIT >> (idx))
#define SETOCCUPIED(pBoard,row,col)        SETOCCUPIEDINDEX(pBoard,(GETINDEX(row,col)))
#define SETUNOCCUPIEDINDEX(pBoard,idx)     (pBoard)->ullCellsInUse = (pBoard->ullCellsInUse & ~((FIRSTBIT) >> (idx)))
#define SETUNOCCUPIED(pBoard,row,col)      SETUNOCCUPIEDINDEX(pBoard,(GETINDEX(row,col)))
#define ISOCCUPIEDINDEXLONG(val,idx)       ((FIRSTBIT >> (idx)) & (val))
#define ISOCCUPIEDLONG(val,row,col)        ISOCCUPIEDINDEXLONG(val,GETINDEX(row,col))
#define ISOCCUPIEDINDEX(pBoard,idx)        ISOCCUPIEDINDEXLONG((pBoard)->ullCellsInUse,idx)
#define ISOCCUPIED(pBoard,row,col)         ISOCCUPIEDLONG((pBoard)->ullCellsInUse,row,col)

/* Colors Macros */
#define BLACK                              ('B')
#define WHITE                              ('W')
#define ISBLACKLONG(val,row,col)           ((FIRSTBIT >> GETINDEX(row,col)) & (val))
#define ISBLACK(pBoard,row,col)            ISBLACKLONG((pBoard)->ullCellColors,row,col)
#define SETWHITE(pBoard,row,col)           (pBoard)->ullCellColors = ((pBoard)->ullCellColors) & ~((FIRSTBIT >> GETINDEX(row,col)))
#define SETBLACK(pBoard,row,col)           (pBoard)->ullCellColors = ((pBoard)->ullCellColors) | (FIRSTBIT >> GETINDEX(row,col))
#define SETCOLOR(pBoard,row,col,color)     if(color == BLACK) {SETBLACK(pBoard,row,col);} else { SETWHITE(pBoard,row,col);}
#define GETCOLOR(pBoard,row,col)           (ISBLACK(pBoard,row,col) ? BLACK : WHITE)
#define GETNUMBLACK(pBoard)                (int) (__popcnt64((((pBoard)->ullCellColors) & (pBoard)->ullCellsInUse)))
#define GETNUMWHITE(pBoard)                (int) (__popcnt64((~((pBoard)->ullCellColors)) & (pBoard)->ullCellsInUse))

/* Next Player Macros */
#define GETBOARDNEXTPLAYERSHORT(val)       (((val) & 0x01) ? BLACK : WHITE)
#define GETBOARDNEXTPLAYER(pBoard)         GETBOARDNEXTPLAYERSHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYERBLACKSHORT(val)  (val) = ((val) | 0x01)
#define SETBOARDNEXTPLAYERWHITESHORT(val)  (val) = ((val) & 0xFE)
#define SETBOARDNEXTPLAYERBLACK(pBoard)    SETBOARDNEXTPLAYERBLACKSHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYERWHITE(pBoard)    SETBOARDNEXTPLAYERWHITESHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYER(pBoard,color)   if(color == BLACK) { SETBOARDNEXTPLAYERBLACK(pBoard); } else { SETBOARDNEXTPLAYERWHITE(pBoard); }
#define SETBOARDNEXTPLAYERFLIP(pBoard)     if(GETBOARDNEXTPLAYER(pBoard) == WHITE) { SETBOARDNEXTPLAYERBLACK(pBoard); } else { SETBOARDNEXTPLAYERWHITE(pBoard); }

/* Possible Moves Macros */
#define SETPOSSIBLEINDEX(pBoard,idx)       (pBoard)->ullPossibleMoves = (((pBoard)->ullPossibleMoves) | (FIRSTBIT >> (idx)))
#define SETPOSSIBLE(pBoard,row,col)        SETPOSSIBLEINDEX(pBoard, GETINDEX(row,col))
#define ISPOSSIBLELONG(val,row,col)		   ((FIRSTBIT >> GETINDEX(row,col)) & (val))	
#define ISPOSSIBLE(pBoard,row,col)         ISPOSSIBLELONG((pBoard)->ullPossibleMoves,row,col)
#define ISPOSSIBLEIDX(pBoard,idx)          ((FIRSTBIT >> (idx)) & ((pBoard)->ullPossibleMoves))
#define GETNUMMOVES(pBoard)                (int) (__popcnt64((pBoard)->ullPossibleMoves))

/* Board States */
#define BOARD_STATE_NOT_PLAYED             0
#define BOARD_STATE_PLAYED_NOT_TERMINAL    1
#define BOARD_STATE_PLAYED_TERMINAL        2
#define BOARD_STATE_PLAYED_NO_MOVES        3

/* Move is player change only */
constexpr auto MOVE_PLAYERCHANGEONLY = 100;


// ── Board-size globals ────────────────────────────────────────────────────────
// Call SetBoardSizeForRun(boardSize) once before any board operations.
// Masks are precomputed here so BoardMoveCalculator avoids rebuilding them
// on every call (which can be billions of times for a full solve).
// Defaults match boardSize=4 so a SetBoardSizeForRun call is not strictly
// required when the board size is 4.
inline int                g_boardSize      = 4;
inline int                g_boardSi        = 2;                    // (8-4)/2
inline int                g_boardEi        = 6;                    // 8-2
inline unsigned long long g_boardLeftEdge  = 0x0000202020200000ULL;
inline unsigned long long g_boardRightEdge = 0x0000040404040000ULL;
inline unsigned long long g_boardMask      = 0x00003C3C3C3C0000ULL;

inline void SetBoardSizeForRun(int boardSize)
{
    g_boardSize      = boardSize;
    g_boardSi        = (8 - boardSize) / 2;
    g_boardEi        = 8 - g_boardSi;
    g_boardLeftEdge  = 0;
    g_boardRightEdge = 0;
    g_boardMask      = 0;
    for (int r = g_boardSi; r < g_boardEi; r++)
    {
        g_boardLeftEdge  |= (FIRSTBIT >> GETINDEX(r, g_boardSi));
        g_boardRightEdge |= (FIRSTBIT >> GETINDEX(r, g_boardEi - 1));
        for (int c = g_boardSi; c < g_boardEi; c++)
            g_boardMask |= (FIRSTBIT >> GETINDEX(r, c));
    }
}

inline int GetMaxMovesForBoardSize(int boardSize)
{
    int returnSize = 0;

    switch (boardSize)
    {
        case 4:
            returnSize = 6;
            break;
        case 6:
			returnSize = 19;
            break;
        case 8:
			returnSize = 28;
            break;
        default:
            Fatal(FATAL_INVALID_BOARD_SIZE, "GetMaxMovesForBoardSize: invalid board size specified (%d)", boardSize);
			returnSize = 0;
    }

	return returnSize;
}

// ─────────────────────────────────────────────────────────────────────────────

int BoardCompare(const void* arg1, const void* arg2);
int BoardCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size);
int BoardKeyCompare(const void* arg1, const void* arg2);
int BoardKeyCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size);
void BoardFlip(PBOARD pBoard, PBOARD pResult);
void BoardRotate90DegreesRight(PBOARD pBoard, PBOARD pResult);
void BoardMirrorVerticalAxis(PBOARD pBoard, PBOARD pResult);
void BoardPrint(FILE* fpOut, int boardCount, ...);
PBOARD BoardAllocateFirstBoard();
PBOARD BoardAllocate();
PBOARD BoardAllocateClone(PBOARD pOrigBoard);
void BoardCreateUniqueBoard(PBOARD pBoard, PBOARD pUniqueBoard, bool *pFlippedBoard, int numRotations = 8);
void BoardMoveCalculator(PBOARD pBoard);
unsigned long long BoardKeyGetMoves(const PBOARD_KEY pKey);

PMOVE MoveAllocate();
void MoveSet(PMOVE pMove, PBOARD pParent, PBOARD pResult, unsigned short usMoveIdx);
void MovePlayAndSetResultBoard(PBOARD pBoard, PBOARD pResultBoard, int row, int col);
void MovePrint(FILE* fpOut, PMOVE pMove);

/* Othello BOARD Return Codes */
constexpr auto RC_BOARD_INVALID_SIZE = RC_BOARD_BASE + 0;
constexpr auto RC_BOARD_ALLOCATE_FAILURE = RC_BOARD_BASE + 1;
constexpr auto RC_BOARD_MOVE_ALLOCATE_FAILURE = RC_BOARD_BASE + 2;
