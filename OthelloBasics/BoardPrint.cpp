#include "OthelloBasics.h"
#include <stdarg.h>
#include <string.h>

/**
    Routine: printRowSeparator
    Parameters:
        int startIdx       - The starting index to print
        int endIdx         - The ending index to print
    Purpose:
     To print a separation row for visual of board
**/
static void printRowSeparator(FILE* fpOut, char startch, int startIdx, int endIdx)
{
    /* Print the row separator */
    fprintf(fpOut, "%c",startch);

    for (int col = startIdx; col < endIdx; col++)
    {
        fprintf(fpOut, "---+");
    }

}

void BoardPrint(FILE* fpOut, int boardCount, ...)
{
#define MAXBOARDPRINT 4
    PBOARD pBoardArray[MAXBOARDPRINT];
    int boardArraySize = (boardCount <= MAXBOARDPRINT ? boardCount : MAXBOARDPRINT);
    char gap[19];


    if (boardArraySize < 1)
        return;


    /* First get the pointers up to max */
    va_list args;

    va_start(args, boardCount);
    for (int i = 0; i < boardArraySize; i++)
    {
        pBoardArray[i] = va_arg(args, PBOARD);
    }
    va_end(args);

    switch (g_boardSize)
    {
        case 4:
            strcpy(gap, "                  ");
            break;
        case 6:
            strcpy(gap, "          ");
            break;
        default:
            strcpy(gap, "  ");
    }

    int startIdx = g_boardSi;
    int endIdx   = g_boardEi;
    char color;

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "Address      =0x%016p", pBoardArray[i]);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "ullCellsInUse=0x%016zX", pBoardArray[i]->ullCellsInUse);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "ullCellColors=0x%016zX", pBoardArray[i]->ullCellColors);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "               ");
        fprintf(fpOut, "usBoardInfo  =0x%04hX", pBoardArray[i]->usBoardInfo);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "ullPossibleMv=0x%016zX", pBoardArray[i]->ullPossibleMoves);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "           ");
        fprintf(fpOut, "ullBlackWins =%010zu", pBoardArray[i]->ullBlackWins);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "           ");
        fprintf(fpOut, "ullWhiteWins =%010zu", pBoardArray[i]->ullWhiteWins);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "           ");
        fprintf(fpOut, "ullTies      =%010zu", pBoardArray[i]->ullTies);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < boardArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "               ");
        fprintf(fpOut, "usBoardState =0x%04hX", pBoardArray[i]->usBoardState);
    }
    fprintf(fpOut, "\n");

    for (int row = startIdx; row < endIdx; row++)
    {
        /* Now loop through all boards to print the separators */
        for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
        {

            if (boardIdx > 0)
                fprintf(fpOut, "%s",gap);

            if (row == startIdx)
                printRowSeparator(fpOut, GETBOARDNEXTPLAYER((pBoardArray[boardIdx])),startIdx,endIdx);
            else
                printRowSeparator(fpOut, '+', startIdx, endIdx);
        }

        fprintf(fpOut, "\n");

        /* Now loop through all boards to print the values */
        for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
        {

            if (boardIdx > 0)
                fprintf(fpOut, "%s", gap);

            /* Print the row's cols*/
            fprintf(fpOut, "|");

            for (int col = startIdx; col < endIdx; col++)
            {
                if (ISOCCUPIED(pBoardArray[boardIdx], row, col))
                {
                    if (ISBLACK(pBoardArray[boardIdx], row, col))
                        color = 'B';
                    else
                        color = 'W';

                }
                else if (ISPOSSIBLE(pBoardArray[boardIdx], row, col))
                {
                    color = '*';
                }
                else
                    color = ' ';

                fprintf(fpOut, " %c |", color);
            }
        }

        fprintf(fpOut, "\n");
    }

    /* Now loop through all boards to print the separators and row contents */
    for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
    {

        if (boardIdx > 0)
            fprintf(fpOut, "%s", gap);

        printRowSeparator(fpOut, '+', startIdx, endIdx);
    }

    fprintf(fpOut, "\n");
}
