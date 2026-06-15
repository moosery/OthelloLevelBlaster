#pragma once
#include <stdio.h>
typedef size_t RC;

constexpr auto RC_SUCCESS = 0;
constexpr auto RC_FATAL_BASE = 0;  /* Additive offset only — fatal codes are RC_FATAL_BASE+1 through 9999; 0 is reserved as RC_SUCCESS */
constexpr auto RC_BOARD_BASE = 10000;
constexpr auto RC_BP_BASE = 20000;
constexpr auto RC_STACK_BASE = 30000;
constexpr auto RC_CACHE_BASE = 40000;
constexpr auto RC_BTP_BASE = 50000;
constexpr auto RC_HS_BASE = 60000;
constexpr auto RC_FS_BASE = 70000;
constexpr auto RC_UTIL_BASE = 80000;
constexpr auto RC_FI_BASE = 90000;

/* FATAL return codes */
constexpr auto FATAL_ALLOCATION_FAILED = RC_FATAL_BASE + 1;
constexpr auto FATAL_BP_DELETE = RC_FATAL_BASE + 2;
constexpr auto FATAL_BP_NODE_EMPTY = RC_FATAL_BASE + 3;
constexpr auto FATAL_BP_DUP_KEY = RC_FATAL_BASE + 4;
constexpr auto FATAL_BP_INTEGRITY_CHECK_FAILED = RC_FATAL_BASE + 5;
constexpr auto FATAL_FILE_OPEN = RC_FATAL_BASE + 6;
constexpr auto FATAL_BP_FIND = RC_FATAL_BASE + 7;
constexpr auto FATAL_MOVE_FIND_FAILED = RC_FATAL_BASE + 8;
constexpr auto FATAL_BOARD_FIND_FAILED = RC_FATAL_BASE + 9;
constexpr auto FATAL_BOARD_NOT_PLAYED = RC_FATAL_BASE + 10;
constexpr auto FATAL_BP_INSERT = RC_FATAL_BASE + 11;
constexpr auto FATAL_BOARD_REPLAY = RC_FATAL_BASE + 12;
constexpr auto FATAL_BAD_BOARD_STATE = RC_FATAL_BASE + 13;
constexpr auto FATAL_READ_CURSOR_LARGER_THAN_WRITE = RC_FATAL_BASE + 14;
constexpr auto FATAL_BOARDS_TO_PROCESS_FAILED = RC_FATAL_BASE + 15;
constexpr auto FATAL_BTP_CHECKPT_FAILED = RC_FATAL_BASE + 16;
constexpr auto FATAL_FS_TOO_MANY_RECORDS_FOR_FILE = RC_FATAL_BASE + 17;
constexpr auto FATAL_FS_BACKUP_FAILED = RC_FATAL_BASE + 18;
constexpr auto FATAL_FS_FILE_LOAD_FAILED = RC_FATAL_BASE + 19;
constexpr auto FATAL_FS_FILE_FREE_FAILED = RC_FATAL_BASE + 20;
constexpr auto FATAL_CACHE_CHECKIN_INVALID = RC_FATAL_BASE + 21;
constexpr auto FATAL_FS_FIND_BEST_FIT_FILE_FAILED = RC_FATAL_BASE + 22;
constexpr auto FATAL_FS_ITERATION_FAILED = RC_FATAL_BASE + 23;
constexpr auto FATAL_BOARD_UPDATE_FAILED = RC_FATAL_BASE + 24;
constexpr auto FATAL_BTP_SORT_WRITE_FAILED = RC_FATAL_BASE + 25;
constexpr auto FATAL_SEEK_FAILED = RC_FATAL_BASE + 26;
constexpr auto FATAL_READ_FAILED = RC_FATAL_BASE + 27;
constexpr auto FATAL_FI_FLUSH_FAILED = RC_FATAL_BASE + 28;
constexpr auto FATAL_TS_UNBALANCED_TREE = RC_FATAL_BASE + 29;
constexpr auto FATAL_MAX_MOVES_EXCEEDED = RC_FATAL_BASE + 30;
constexpr auto FATAL_DRIVE_CACHE_WRITE_FAILED = RC_FATAL_BASE + 31;
constexpr auto FATAL_CREATE_DIR_FAILED        = RC_FATAL_BASE + 32;
constexpr auto FATAL_INSUFFICIENT_MEMORY = RC_FATAL_BASE + 33;
constexpr auto FATAL_INVALID_BOARD_SIZE = RC_FATAL_BASE + 34;
constexpr auto FATAL_DRIVE_SPACE        = RC_FATAL_BASE + 35;
constexpr auto FATAL_GPU_ERROR          = RC_FATAL_BASE + 36;

/* Utility Implementation errors */
constexpr auto  UTIL_RC_Success = RC_SUCCESS;
constexpr auto  UTIL_RC_Could_Not_Create_Directory = RC_UTIL_BASE + 1;
constexpr auto  UTIL_RC_Path_Too_Long = RC_UTIL_BASE + 2;

void Error(RC error, const char* pszReasonFmt, ...);
RC ErrorGetLast();
char* ErrorGetLastReason();
void ErrorPrint(FILE* fpOut);

void Fatal(RC rc, const char* pszReasonFmt, ...);
