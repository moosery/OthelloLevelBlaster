#pragma once
#include <stdio.h>
#include "Mem.h"
#include <ArenaMem.h>
#include "RWLock.h"
#include "Error.h"
#include <vector>

//#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG_INSERT		0x0000000000000001
#define DEBUG_INSERT_PATH	0x0000000000000002
#define DEBUG_DELETE        0x0000000000000004
#define DEBUG_LOCKS         0x0000000000000008
#define DEBUG_SEARCH        0x0000000000000010
#ifdef BP_CREATE_TREE
size_t debugWhat = DEBUG_LOCKS;
FILE* fpDebug = fopen("D:\\DebugBP.txt", "w");
/* Guard struct: destructor runs at program exit and closes fpDebug. */
static struct BPDebugFileGuard
{
    ~BPDebugFileGuard() { if (fpDebug) { fclose(fpDebug); fpDebug = NULL; } }
} bpDebugFileGuard;
#else
extern size_t debugWhat;
extern FILE* fpDebug;
#endif
#define IsDebugging(flag)   ((flag) & debugWhat)
#endif

/* Alignement size (2 byte, 4 byte, or 8 byte) */
#define BP_ALIGNMENT_SIZE					8
#define BP_ALIGNED_SIZE(size)				(((size) + BP_ALIGNMENT_SIZE - 1) & ~(BP_ALIGNMENT_SIZE - 1))

/* Node Information Flags */
#define BP_NODETYPE_MASK                    0x0000000000000003
#define BP_NODEINFO_LEAF					0x0000000000000001
#define BP_NODEINFO_KEY						0x0000000000000002


typedef size_t BPRc;
typedef long long BPLL;

/* The following datatypes are variable in size */
#define BP_IDX_DATATYPE_CHAR		0
#define BP_IDX_DATATYPE_BYTE		1
#define BP_IDX_DATATYPE_BINARY		2

/* The num types below are stored in the machine specific format (ENDIAN) */
/* On this machine:                                                       */
/*  2 bytes = short                                                       */
/*  4 bytes = long                                                        */
/*  8 bytes = BPLL                                                   */
#define BP_IDX_DATATYPE_SNUM_2BYTE		3
#define BP_IDX_DATATYPE_UNUM_2BYTE		4
#define BP_IDX_DATATYPE_SNUM_4BYTE		5
#define BP_IDX_DATATYPE_UNUM_4BYTE		6
#define BP_IDX_DATATYPE_SNUM_8BYTE		7
#define BP_IDX_DATATYPE_UNUM_8BYTE		8

#define BP_IDX_MAX_KEY_FLDS		32
#define BP_IDX_SETTING_DEFAULT     0x0000000000000000
#define BP_IDX_SETTING_SORT_DESC   0x0000000000000001
#define BP_IDX_SETTINGS_MASK       0x0000000000000001

#define BP_IDX_MAX_DATA_DEFAULT    0x3FFFFFFFFFFFFFFF


#define BP_SEARCHTYPE_GET_FIRST									1
#define BP_SEARCHTYPE_GET_LAST									2
#define BP_SEARCHTYPE_GET_NEXT									3
#define BP_SEARCHTYPE_GET_PREV									4
#define BP_SEARCHTYPE_GET_EQUAL_KEY								5
#define BP_SEARCHTYPE_GET_JUST_LESS_THAN_OR_EQUAL_KEY			6
#define BP_SEARCHTYPE_GET_GREATER_OR_EQUAL_KEY					7


typedef struct _BPNode
{
	RWLock		rwNodeLock;
	size_t		stNodeInfo;
	BPLL		llNumInNode;
	_BPNode* pParent;
	_BPNode* pLeftSibling;
	_BPNode* pRightSibling;
	char** ppDataPtrArray;
	_BPNode** ppChildArray;
} BPNode, * PBPNode;

typedef struct _BPIdxFld
{
	size_t		stDataOffset;
	size_t		stLength;
	size_t		stDataType;
}BPIdxFld, * PBPIdxFld;

typedef struct _BPIdxInfo
{
	RWLock		rwIdxLock;
	size_t      stMaxDataCnt;
	size_t		stDataCnt;							/* Current number of data items stored in the tree	*/
	size_t      stIdxSettings;
	PBPNode		pRootNode;
	size_t      stNumFlds;
	size_t		stNumKeyNodes;
	size_t		stNumLeafNodes;
	size_t		stKeyNodeSize;
	size_t		stLeafNodeSize;
	BPLL		stIdxDepth;							/* The depth of the tree (min = 1)				*/
	BPIdxFld    idxFlds[BP_IDX_MAX_KEY_FLDS];
} BPIdxInfo, * PBPIdxInfo;

typedef struct _BPTree
{
	RWLock		rwTreeLock;
    PArenaMem   pArena;								/* The memory arena for this tree (NULL = malloc mode) */
	size_t		stDataSize;							/* The size of the user data area				*/
	BPLL		llOrder;							/* Number of pointers (1 more than keys)        */
	BPLL		llMinKeys;							/* Min number of keys per node                  */
	BPIdxInfo	keyInfo;							/* The key information for this tree			*/
} BPTree, * PBPTree;

typedef struct _BPIterator
{
	bool isDone;
	char _pad[7];                       // explicit padding: bool is 1 byte; PBPNode needs 8-byte alignment at byte 8
	PBPNode currNode;
	PBPTree pTree;
	BPLL nxtIdx;
} BPIterator, * PBPIterator;


/* Routines */

void BPIdxInfoInit(PBPIdxInfo pInfo, size_t optionalSettings);
BPRc BPIdxInfoAddFld(PBPIdxInfo pInfo, size_t stDataType, size_t stFldSize, size_t stFldOffset);


void BPFreeNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode);
void BPFreeNodeButNotData(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode);
BPRc BPAllocateNode(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode* ppNode, size_t nodeType);

void BPFreeTree(PBPTree pTree, bool freeData);
BPRc BPCreateTree(PBPTree* ppTree, BPLL llOrder, size_t maxMemoryBytes, size_t stIdxSettings, size_t stNumFlds, BPIdxFld idxFlds[], size_t stDataSize, PArenaMem pArena = nullptr);

void BPPrintTree(FILE* fpOut, PBPTree pTree);
void BPPrintNode(FILE* fpOut, PBPTree pTree, PBPNode pNode);
void BPPrintKeyAtAddress(FILE* fpOut, PBPTree pTree, void* ptrDatav);

int BPKeyCmpPP(PBPTree pTree, PBPIdxInfo pIdxInfo, void* p1, void* p2);
int BPKeyCmpPPRaw(size_t stNumFlds, size_t stIdxSettings, BPIdxFld idxFlds[], const void* p1, const void* p2);

size_t BPGetDataCnt(PBPTree pTree);

BPRc BPInsertDataPtr(PBPTree pTree, void* pData);
BPRc BPInsertCopy(PBPTree pTree, void* pData);

bool BPIntegrityCheck(FILE* fpOut, PBPTree pTree);

void BPPrintTreeHeader(FILE* fpOut, PBPTree pTree);

BPRc BPDeleteDataItem(PBPTree pTree, void* pData, bool freeData);
BPRc BPDeleteDataNoFree(PBPTree pTree, void* pData);
BPRc BPDeleteDataAndFree(PBPTree pTree, void* pData);

BPRc BPFindEqualKey(PBPTree pTree, void* pKeyData, void* pDataFound);
BPRc BPFindFirstKey(PBPTree pTree, void* pDataFound);
BPRc BPFindLastKey(PBPTree pTree,  void* pDataFound);
BPRc BPFindGreaterThanKey(PBPTree pTree, void* pKeyData, void* pDataFound, bool returnEqual);
BPRc BPFindLessThanKey(PBPTree pTree, void* pKeyData, void* pDataFound, bool returnEqual);
BPLL BPFindNodeDataBinary(PBPTree pTree, PBPIdxInfo pIdxInfo, PBPNode pNode, void* pData);

void BPIterateStart(PBPTree pTree, PBPIterator pIterator);
void BPIterateStartFrom(PBPTree pTree, PBPIterator pIterator, void* pKeyData, bool returnEqual);
BPRc BPIterate(PBPIterator pIterator, void* pDataFound);
void BPIterateStop(PBPIterator pIterator);

BPRc BPUpdate(PBPTree pTree, void* pDataToUpdate);

BPRc BPGetLevelStartKeys(BPTree* pTree, PBPIdxInfo pIdxInfo, BPLL targetNodeCount, std::vector<void*>& outKeys);

/* Macros */
#ifndef min
#define min(a,b)								((b) < (a) ? (b) : (a))
#define max(a,b)								((b) > (a) ? (b) : (a))
#endif

#define SizeofDataEntry(pTree)					(max(BP_ALIGNED_SIZE((pTree)->stDataSize),sizeof(size_t)))
#define BPIsKeyNode(pNode)						((pNode->stNodeInfo) & BP_NODEINFO_KEY)
#define BPIsLeafNode(pNode)						(!(BPIsKeyNode(pNode)))
#define NodeTypeFlag(pNode)						((pNode)->stNodeInfo & BP_NODETYPE_MASK)

/* BPlus Tree Error codes*/
constexpr auto BP_RC_Success = RC_SUCCESS;
constexpr auto BP_RC_Idx_Datatype_Invalid = RC_BP_BASE + 1;
constexpr auto BP_RC_Max_Idx_Defined = RC_BP_BASE + 2;
constexpr auto BP_RC_Allocate_Failed = RC_BP_BASE + 3;
constexpr auto BP_RC_Invalid_Settings = RC_BP_BASE + 4;
constexpr auto BP_RC_Invalid_Num_Fields = RC_BP_BASE + 5;
constexpr auto BP_RC_Invalid_Data_Offset = RC_BP_BASE + 6;
constexpr auto BP_RC_Invalid_Data_Ptr = RC_BP_BASE + 7;
constexpr auto BP_RC_Duplicate_Found = RC_BP_BASE + 8;
constexpr auto BP_RC_Not_Found = RC_BP_BASE + 9;
constexpr auto BP_RC_Deadlock_Prevention = RC_BP_BASE + 10;
constexpr auto BP_RC_Invalid_Search_Type = RC_BP_BASE + 11;
constexpr auto BP_RC_Tree_Full = RC_BP_BASE + 12;
