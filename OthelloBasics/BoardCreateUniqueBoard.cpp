#include "OthelloBasics.h"
#include "BinSearchLE.h"
#include <stdlib.h>
#include <memory.h>
#include "Rotation.h"

void BoardCreateUniqueBoard(PBOARD pBoard, PBOARD pUniqueBoard, bool *pFlippedBoard, int numRotations)
{
    BOARD boardArray[16];

    memset(boardArray, 0, sizeof(BOARD) * numRotations);
    memcpy(&(boardArray[0]), pBoard, offsetof(BOARD, ullPossibleMoves));

    if (numRotations >= 4)
    {
        BoardRotate90DegreesRight(&(boardArray[0]), &(boardArray[1]));
        BoardRotate90DegreesRight(&(boardArray[1]), &(boardArray[2]));
        BoardRotate90DegreesRight(&(boardArray[2]), &(boardArray[3]));
    }

    if (numRotations >= 8)
    {
        BoardMirrorVerticalAxis(&(boardArray[0]), &(boardArray[4]));
        BoardRotate90DegreesRight(&(boardArray[4]), &(boardArray[5]));
        BoardRotate90DegreesRight(&(boardArray[5]), &(boardArray[6]));
        BoardRotate90DegreesRight(&(boardArray[6]), &(boardArray[7]));
    }

    if (numRotations == 16)
    {
        BoardFlip(&(boardArray[0]), &(boardArray[8]));
        BoardRotate90DegreesRight(&(boardArray[8]), &(boardArray[9]));
        BoardRotate90DegreesRight(&(boardArray[9]), &(boardArray[10]));
        BoardRotate90DegreesRight(&(boardArray[10]), &(boardArray[11]));
        BoardMirrorVerticalAxis(&(boardArray[8]), &(boardArray[12]));
        BoardRotate90DegreesRight(&(boardArray[12]), &(boardArray[13]));
        BoardRotate90DegreesRight(&(boardArray[13]), &(boardArray[14]));
        BoardRotate90DegreesRight(&(boardArray[14]), &(boardArray[15]));
    }

    if (numRotations > 1)
    {
        qsort(boardArray, numRotations, sizeof(BOARD), BoardCompare);
    }


    BoardMoveCalculator(&(boardArray[0]));
    memcpy(pUniqueBoard, &(boardArray[0]), sizeof(BOARD));

    *pFlippedBoard = (GETBOARDNEXTPLAYER(pUniqueBoard) != GETBOARDNEXTPLAYER(pBoard) ? true : false);
}


