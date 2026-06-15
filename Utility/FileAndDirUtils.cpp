#include <stdio.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include "Error.h"
#include "Utility.h"

static bool createPath(char* pPathToCreate)
{
	int mkDirResult = _mkdir(pPathToCreate);
	bool result = true;

	if (mkDirResult != 0 && errno != EEXIST)
	{
		Error(UTIL_RC_Could_Not_Create_Directory, "The directory '%s' cannot be created (%d)", pPathToCreate, errno);
		result = false;
	}

	return result;
}

bool CreateFullPath(const char* pszFullPath)
{
	bool result = true;
	char tempName[MAX_FULL_PATH_NAME + 1];
	size_t nameLen = strlen(pszFullPath);
	int currIdx = 0;
	memset(tempName, 0, sizeof(tempName));
	bool hadADirName = false;

	if (nameLen > MAX_FULL_PATH_NAME)
	{
		result = false;
		Error(UTIL_RC_Path_Too_Long,"The path '%s' is larger than allowed (%zu)\n",pszFullPath,(size_t) MAX_FULL_PATH_NAME);
	}
	else
	{
		if (nameLen >= 2 && pszFullPath[1] == ':')
		{
			currIdx = 2;
			tempName[0] = pszFullPath[0];
			tempName[1] = pszFullPath[1];
			if (nameLen >= 3 && (pszFullPath[2] == '\\' || pszFullPath[2] == '/'))
			{
				currIdx = 3;
				tempName[2] = '\\';
			}
		}
	}

	for (; currIdx <= nameLen && result; currIdx++)
	{
		switch (pszFullPath[currIdx])
		{
			case '\0':
			case '\\':
			case '/':
				if (hadADirName)
					result = createPath(tempName);
				tempName[currIdx] = '\\';
				break;
			default:
				tempName[currIdx] = pszFullPath[currIdx];
				hadADirName = true;
		}
	}

	return result;
}