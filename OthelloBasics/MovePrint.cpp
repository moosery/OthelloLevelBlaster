#include "OthelloBasics.h"
#include <stdio.h>
#include <algorithm>

static void printRowSeparator(FILE* fpOut, char startch, int startIdx, int endIdx)
{
    /* Print the row separator */
    fprintf(fpOut, "%c", startch);

    for (int col = startIdx; col < endIdx; col++)
    {
        fprintf(fpOut, "---+");
    }

}

static void movePrintBoardPart(FILE *fpOut, const char* pName, size_t ullCellsInUse, size_t ullCellColors, unsigned short usBoardInfo, unsigned short usRow, unsigned short usCol)
{
    /* Print the board in visual form */
    int startIdx = g_boardSi;
    int endIdx = g_boardEi;
    char color;
    bool isParent = (strcmp(pName, "Parent") == 0);

    fprintf(fpOut, "The move's %s board is as follows:\n", pName);
    if (__popcnt64(ullCellsInUse) == 0)
    {
        fprintf(fpOut, "Not Calculated Yet!!!!\n");
        return;
    }

    for (int row = startIdx; row < endIdx; row++)
    {
        if (row == startIdx)
            printRowSeparator(fpOut, GETBOARDNEXTPLAYERSHORT(usBoardInfo), startIdx, endIdx);
        else
            printRowSeparator(fpOut, '+', startIdx, endIdx);

        /* Print the row's cols*/
        fprintf(fpOut, "|");

        for (int col = startIdx; col < endIdx; col++)
        {
            if (ISOCCUPIEDLONG(ullCellsInUse, row, col))
            {
                if (ISBLACKLONG(ullCellColors, row, col))
                    color = 'B';
                else
                    color = 'W';

            }
            else
            {
                if (isParent && row == usRow && col == usCol)
                    color = '*';
                else
                    color = ' ';
            }

            fprintf(fpOut, " %c |", color);
        }

        fprintf(fpOut, "\n");
    }

    printRowSeparator(fpOut, '+', startIdx, endIdx);

    /* Now dump the raw data */
    fprintf(stdout, " ullCellsInUse      : 0x%016llx\n", ullCellsInUse);
    fprintf(stdout, " ullCellColors      : 0x%016llx\n", ullCellColors);
    fprintf(stdout, " usBoardInfo        : 0x%02x\n", usBoardInfo);
    fprintf(stdout, "   size             : %d\n", g_boardSize);
    fprintf(stdout, "   nextPlayer       : %c\n", GETBOARDNEXTPLAYERSHORT(usBoardInfo));



}

void MovePrint(FILE *fpOut, PMOVE pMove)
{
    fprintf(fpOut, "MOVE: address = 0x%016p\n", pMove);
    fprintf(fpOut, "      fullKey = 0x");
    char* pData = (char*)pMove;
    for (size_t tmpIdx = 0; tmpIdx < offsetof(MOVE,ullCellsInUseResult); tmpIdx++, pData++)
    {
        unsigned int value = (unsigned int)(*((unsigned char*)pData)) & 0xFF;
        fprintf(fpOut, "%02X", value);
    }
    fprintf(fpOut, "\n");
    fprintf(fpOut, "      rsltIdx = 0x");
    pData = (char*)&(pMove->ullCellsInUseResult);
    for (size_t tmpIdx = 0; tmpIdx < 18; tmpIdx++, pData++)
    {
        unsigned int value = (unsigned int)(*((unsigned char*)pData)) & 0xFF;
        fprintf(fpOut, "%02X", value);
    }
    fprintf(fpOut, "\n");

    /* Exit if the pointer is bad */
    if (pMove == NULL)
        return;

    /* Now dump the raw data */
    fprintf(fpOut, " usMoveIdx          : %d\n", pMove->usMoveIdx);
    fprintf(fpOut, " move Row           : %d (%d)\n", GETROWFROMINDEX(pMove->usMoveIdx), (GETROWFROMINDEX(pMove->usMoveIdx) - (g_boardSi) + 1));
    fprintf(fpOut, " move Col           : %d (%d)\n", GETCOLFROMINDEX(pMove->usMoveIdx), (GETCOLFROMINDEX(pMove->usMoveIdx) - (g_boardSi) + 1));
    fprintf(fpOut, " ullCellsInUseParent: 0x%016zX\n", pMove->ullCellsInUseParent);
    fprintf(fpOut, " ullCellColorsParent: 0x%016zX\n", pMove->ullCellColorsParent);
    fprintf(fpOut, " usBoardInfoParent  : 0x%02hX\n", pMove->usBoardInfoParent);
    fprintf(fpOut, " ullCellsInUseResult: 0x%016zX\n", pMove->ullCellsInUseResult);
    fprintf(fpOut, " ullCellColorsResult: 0x%016zX\n", pMove->ullCellColorsResult);
    fprintf(fpOut, " usBoardInfoResult  : 0x%02hX\n", pMove->usBoardInfoResult);

    /* Create three temporary boards: the parent, the result unique new board, and the result in correct orientation */
    BOARD parent;
    BOARD result;

    memset(&parent, 0, sizeof(BOARD));
    memset(&result, 0, sizeof(BOARD));

    parent.ullCellsInUse = pMove->ullCellsInUseParent;
    parent.ullCellColors = pMove->ullCellColorsParent;
    parent.usBoardInfo = pMove->usBoardInfoParent;

    result.ullCellsInUse = pMove->ullCellsInUseResult;
    result.ullCellColors = pMove->ullCellColorsResult;
    result.usBoardInfo = pMove->usBoardInfoResult;


    fprintf(fpOut, " Parent/Unique Result/Oriented Result\n");
    BoardPrint(fpOut, 2, &parent, &result);
    fprintf(fpOut, "\n");
}

