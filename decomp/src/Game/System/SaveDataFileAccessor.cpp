#include "Game/System/SaveDataFileAccessor.hpp"
#include "Game/Util/StringUtil.hpp"
#include <cstring>

SaveDataFileAccessor::SaveDataFileAccessor(u8* pSaveDataFile) : mFile(reinterpret_cast< SaveDataFile* >(pSaveDataFile)) {
}

SaveDataFileHeader* SaveDataFileAccessor::getHeader() {
    return &mFile->mHeader;
}

SaveDataFileInfo* SaveDataFileAccessor::getFileInfo(int index) {
    return &mFile->mInfo[index];
}

void SaveDataFileAccessor::makeUserFileInfo(SaveDataUserFileInfo* pUserFileInfo, const char* pName) {
    pUserFileInfo->mData = nullptr;
    pUserFileInfo->mDataSize = 0;
    pUserFileInfo->mKind = 1;

    SaveDataFileHeader* pHeader = getHeader();
    SaveDataFileInfo* pInfo = getFileInfo(0);

    if (pHeader->mFileSize > 0x10000 || pHeader->mUserFileInfoNum > 24) {
        return;
    }

    for (u32 i = 0; i < pHeader->mUserFileInfoNum; i++) {
        if (!MR::isEqualString(pInfo[i].mName, pName)) {
            continue;
        }

        u32 startOffset = pInfo[i].mOffset;
        u32 endOffset = (i == pHeader->mUserFileInfoNum - 1) ? pHeader->mFileSize : pInfo[i + 1].mOffset;
        if (startOffset > pHeader->mFileSize || endOffset < startOffset || endOffset > pHeader->mFileSize) {
            continue;
        }

        pUserFileInfo->mDataSize = endOffset - startOffset;
        pUserFileInfo->mData = reinterpret_cast< u8* >(getHeader()) + startOffset;

        if (strstr(pInfo[i].mName, "mario") != nullptr || strstr(pInfo[i].mName, "luigi") != nullptr) {
            pUserFileInfo->mKind = 0;
        }

        if (strstr(pInfo[i].mName, "sysconf") != nullptr) {
            pUserFileInfo->mKind = 2;
        }
    }
}
