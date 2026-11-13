#include "Game/System/SaveDataHandler.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/System/NANDManager.hpp"
#include "Game/System/SaveDataBannerCreator.hpp"
#include "Game/System/SaveDataFileAccessor.hpp"
#include "Game/System/SysConfigFile.hpp"
#include "Game/System/UserFile.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <cstdio>
#include <cstring>

namespace {
    struct SaveFileSpec {
        /* 0x00 */ const char* mName;
        /* 0x04 */ u32 mBufferSize;
    };

    const char cSaveFileName[] = "GameData.bin";
    const char cBannerFileName[] = "banner.bin";
    const SaveFileSpec cSaveFileSpecTable[] = {{"mario", 0xF80}, {"luigi", 0xF80}, {"config", 0x60}};
    const SaveFileSpec cSaveFileSpecSystem = {"sysconf", 0x80};

    inline u16 readBE16(const u8* p) {
        return static_cast< u16 >((static_cast< u16 >(p[0]) << 8) | static_cast< u16 >(p[1]));
    }

    inline u32 readBE32(const u8* p) {
        return (static_cast< u32 >(p[0]) << 24) | (static_cast< u32 >(p[1]) << 16) |
               (static_cast< u32 >(p[2]) << 8) | static_cast< u32 >(p[3]);
    }

    inline u64 readBE64(const u8* p) {
        return (static_cast< u64 >(readBE32(p)) << 32) | static_cast< u64 >(readBE32(p + 4));
    }

    inline void writeLE16(u8* p, u16 v) {
        p[0] = static_cast< u8 >(v & 0xFF);
        p[1] = static_cast< u8 >((v >> 8) & 0xFF);
    }

    inline void writeLE32(u8* p, u32 v) {
        p[0] = static_cast< u8 >(v & 0xFF);
        p[1] = static_cast< u8 >((v >> 8) & 0xFF);
        p[2] = static_cast< u8 >((v >> 16) & 0xFF);
        p[3] = static_cast< u8 >((v >> 24) & 0xFF);
    }

    inline void writeLE64(u8* p, u64 v) {
        writeLE32(p, static_cast< u32 >(v & 0xFFFFFFFFu));
        writeLE32(p + 4, static_cast< u32 >((v >> 32) & 0xFFFFFFFFu));
    }

    inline u16 swapBE16InPlace(u8* p) {
        u16 v = readBE16(p);
        writeLE16(p, v);
        return v;
    }

    void swapU16ArrayInPlace(u8* pBuf, u32 byteSize) {
        for (u32 off = 0; off + 2 <= byteSize; off += 2) {
            swapBE16InPlace(pBuf + off);
        }
    }

    bool convertBEContentAccessorHeader(u8* pBuf, u32 maxSize, u16* pOutAttrNum, u16* pOutDataSize,
                                        u32* pOutHeaderSize, const u8** pOutAttrTable) {
        if (maxSize < 4) {
            return false;
        }
        u16 attrNum = swapBE16InPlace(pBuf);
        u16 dataSize = swapBE16InPlace(pBuf + 2);
        u32 headerSize = 4u + static_cast< u32 >(attrNum) * 4u;
        if (headerSize > maxSize) {
            return false;
        }
        for (u32 i = 0; i < attrNum; i++) {
            u8* entry = pBuf + 4u + i * 4u;
            swapBE16InPlace(entry);      // u16 mHash
            swapBE16InPlace(entry + 2);  // u16 mOffset
        }
        *pOutAttrNum = attrNum;
        *pOutDataSize = dataSize;
        *pOutHeaderSize = headerSize;
        *pOutAttrTable = pBuf + 4u;
        return true;
    }

    bool findContentAttrOffset(const u8* pAttrTable, u16 attrNum, const char* pAttrName, u16* pOutOffset) {
        u16 targetHash = static_cast< u16 >(MR::getHashCode(pAttrName));
        for (u16 i = 0; i < attrNum; i++) {
            const u8* entry = pAttrTable + static_cast< u32 >(i) * 4u;
            u16 hash;
            u16 offset;
            std::memcpy(&hash, entry, sizeof(u16));
            std::memcpy(&offset, entry + 2, sizeof(u16));
            if (hash == targetHash) {
                *pOutOffset = offset;
                return true;
            }
        }
        return false;
    }

    void convertBEGalaxyChunkPayload(u8* pPayload, u32 payloadSize) {
        if (payloadSize < 6) {
            return;
        }
        u16 galaxyNum = swapBE16InPlace(pPayload);
        u16 attrNum = 0;
        u16 dataSize = 0;
        u32 headerSize = 0;
        const u8* pAttrTable = nullptr;
        if (!convertBEContentAccessorHeader(pPayload + 2, payloadSize - 2, &attrNum, &dataSize, &headerSize, &pAttrTable)) {
            return;
        }
        if (dataSize == 0) {
            return;
        }
        u16 nameOff = 0;
        bool hasName = findContentAttrOffset(pAttrTable, attrNum, "mGalaxyName", &nameOff);
        u16 coinOff = 0;
        bool hasCoin = findContentAttrOffset(pAttrTable, attrNum, "mMaxCoinNum", &coinOff);

        u32 recordsStart = 2u + headerSize;
        for (u32 i = 0; i < galaxyNum; i++) {
            u32 recOff = recordsStart + i * static_cast< u32 >(dataSize);
            if (recOff + dataSize > payloadSize) {
                break;
            }
            if (hasName && static_cast< u32 >(nameOff) + 2u <= dataSize) {
                swapBE16InPlace(pPayload + recOff + nameOff);
            }
            if (hasCoin && static_cast< u32 >(coinOff) + 16u <= dataSize) {
                swapU16ArrayInPlace(pPayload + recOff + coinOff, 16u);
            }
        }
    }

    void convertBEPlayerStatusPayload(u8* pPayload, u32 payloadSize) {
        if (payloadSize < 7) {
            return;
        }
        // [0] u8 mStoryProgress, [1..4] u32 mStockedStarPiece, [5..6] u16 mPlayerLeftSupply
        writeLE32(pPayload + 1, readBE32(pPayload + 1));
        writeLE16(pPayload + 5, readBE16(pPayload + 5));
    }

    void convertBESpinDriverPathPayload(u8* pPayload, u32 payloadSize) {
        if (payloadSize < 1) {
            return;
        }
        u8 numGalaxies = pPayload[0];
        u32 pos = 1;
        for (u8 i = 0; i < numGalaxies; i++) {
            if (pos + 6 > payloadSize) {
                break;
            }
            swapBE16InPlace(pPayload + pos);  // u16 galaxyHash
            u16 blockSize = swapBE16InPlace(pPayload + pos + 2);
            if (blockSize < 6 || pos + blockSize > payloadSize) {
                break;
            }
            u8 numScenarios = pPayload[pos + 4];
            u32 spos = pos + 6;
            for (u8 j = 0; j < numScenarios; j++) {
                if (spos + 2 > pos + blockSize) {
                    break;
                }
                u16 scenarioBlockSize = swapBE16InPlace(pPayload + spos);
                if (scenarioBlockSize < 2 || spos + scenarioBlockSize > pos + blockSize) {
                    break;
                }
                spos += scenarioBlockSize;
            }
            pos += blockSize;
        }
    }

    void convertBESysConfigPayload(u8* pPayload, u32 payloadSize) {
        u16 attrNum = 0;
        u16 dataSize = 0;
        u32 headerSize = 0;
        const u8* pAttrTable = nullptr;
        if (!convertBEContentAccessorHeader(pPayload, payloadSize, &attrNum, &dataSize, &headerSize, &pAttrTable)) {
            return;
        }
        if (headerSize + dataSize > payloadSize) {
            return;
        }
        u8* pData = pPayload + headerSize;
        u16 off = 0;
        if (findContentAttrOffset(pAttrTable, attrNum, "mTimeAnnounced", &off) && static_cast< u32 >(off) + 8u <= dataSize) {
            writeLE64(pData + off, readBE64(pData + off));
        }
        if (findContentAttrOffset(pAttrTable, attrNum, "mTimeSent", &off) && static_cast< u32 >(off) + 8u <= dataSize) {
            writeLE64(pData + off, readBE64(pData + off));
        }
        if (findContentAttrOffset(pAttrTable, attrNum, "mSentBytes", &off) && static_cast< u32 >(off) + 4u <= dataSize) {
            writeLE32(pData + off, readBE32(pData + off));
        }
    }

    void convertBEChunkPayload(u32 sig, u8* pPayload, u32 payloadSize) {
        switch (sig) {
        case 'GALA':
            convertBEGalaxyChunkPayload(pPayload, payloadSize);
            break;
        case 'FLG1':
        case 'VLE1':
        case 'PCE1':
            swapU16ArrayInPlace(pPayload, payloadSize);
            break;
        case 'PLAY':
            convertBEPlayerStatusPayload(pPayload, payloadSize);
            break;
        case 'SPN1':
            convertBESpinDriverPathPayload(pPayload, payloadSize);
            break;
        case 'MISC':
            if (payloadSize >= 9) {
                writeLE64(pPayload + 1, readBE64(pPayload + 1));
            }
            break;
        case 'SYSC':
            convertBESysConfigPayload(pPayload, payloadSize);
            break;
        default:
            // 'CONF', 'MII ' contain only 1-byte fields and opaque byte arrays.
            break;
        }
    }

    void convertBESubfileChunks(u8* pSub, u32 subSize) {
        if (subSize < 4 || pSub[0] != 1 || pSub[1] == 0) {
            return;
        }
        u8 numChunks = pSub[1];
        u32 off = 4;
        for (u8 idx = 0; idx < numChunks; idx++) {
            if (off + 12 > subSize) {
                break;
            }
            u32 sig = readBE32(pSub + off);
            u32 hash = readBE32(pSub + off + 4);
            u32 chunkSize = readBE32(pSub + off + 8);
            if (chunkSize < 12 || off + chunkSize > subSize) {
                break;
            }
            writeLE32(pSub + off, sig);
            writeLE32(pSub + off + 4, hash);
            writeLE32(pSub + off + 8, chunkSize);
            convertBEChunkPayload(sig, pSub + off + 12, chunkSize - 12);
            off += chunkSize;
        }
    }

    bool isValidSaveHeaderFields(u32 version, u32 fileSize, u32 infoNum, u32 validBytes) {
        if (version != 2 || infoNum == 0 || infoNum >= 24) {
            return false;
        }
        u32 minHeaderSize = static_cast< u32 >(sizeof(SaveDataFileHeader)) + infoNum * static_cast< u32 >(sizeof(SaveDataFileInfo));
        if (fileSize < minHeaderSize || fileSize > validBytes || fileSize > 0x10000) {
            return false;
        }
        return true;
    }

    bool convertGameDataBufferBEtoLE(u8* pBuffer, u32 validBytes) {
        u32 checkSum = readBE32(pBuffer + 0);
        u32 version = readBE32(pBuffer + 4);
        u32 infoNum = readBE32(pBuffer + 8);
        u32 fileSize = readBE32(pBuffer + 12);
        if (!isValidSaveHeaderFields(version, fileSize, infoNum, validBytes)) {
            return false;
        }

        writeLE32(pBuffer + 0, checkSum);
        writeLE32(pBuffer + 4, version);
        writeLE32(pBuffer + 8, infoNum);
        writeLE32(pBuffer + 12, fileSize);

        u32 minDataStart = static_cast< u32 >(sizeof(SaveDataFileHeader)) + infoNum * static_cast< u32 >(sizeof(SaveDataFileInfo));
        for (u32 i = 0; i < infoNum; i++) {
            u8* pEntry = pBuffer + sizeof(SaveDataFileHeader) + i * sizeof(SaveDataFileInfo);
            u32 offset = readBE32(pEntry + 12);
            writeLE32(pEntry + 12, offset);
        }

        const SaveDataFile* pFile = reinterpret_cast< const SaveDataFile* >(pBuffer);
        for (u32 i = 0; i < infoNum; i++) {
            u32 start = pFile->mInfo[i].mOffset;
            u32 end = (i == infoNum - 1) ? fileSize : pFile->mInfo[i + 1].mOffset;
            if (start < minDataStart || start > fileSize || end < start || end > fileSize) {
                continue;
            }
            convertBESubfileChunks(pBuffer + start, end - start);
        }
        return true;
    }
};  // namespace

namespace {
    NEW_NERVE(SaveDataHandlerWait, SaveDataHandler, Wait);
    NEW_NERVE(SaveDataHandlerProcessing, SaveDataHandler, Processing);
    NEW_NERVE(SaveDataHandlerSaveProcessingGameData, SaveDataHandler, SaveProcessingGameData);
    NEW_NERVE(SaveDataHandlerSaveProcessingBanner, SaveDataHandler, SaveProcessingBanner);
    NEW_NERVE(SaveDataHandlerRemoveProcessingBanner, SaveDataHandler, RemoveProcessingBanner);
    NEW_NERVE(SaveDataHandlerRemoveProcessingGameData, SaveDataHandler, RemoveProcessingGameData);
};  // namespace

SaveDataHandler::SaveDataHandler(const SysConfigFile* pSysConfigFile, const UserFile* pUserFile)
    : NerveExecutor("SaveDataHandler"), mNANDRequestInfo(), _C(), _10(), _14(), _18(), mBannerCreator() {
    mNANDRequestInfo = new NANDRequestInfo();

    createCommunicationBuffer();
    resetSaveData(_18);
    initializeAllFileInSaveData(_18, pSysConfigFile, pUserFile);

    mBannerCreator = new SaveDataBannerCreator();

    initNerve(GET_NERVE_ANON(SaveDataHandlerWait));
}

void SaveDataHandler::update() {
    updateNerve();
    mBannerCreator->updateNerve();
}

void SaveDataHandler::requestCheckEnableToCreate() {
    MR::addRequestToNANDManager(mNANDRequestInfo->setCheck(5, 2, &_10));

    setNerve(GET_NERVE_ANON(SaveDataHandlerProcessing));
}

void SaveDataHandler::requestLoadSaveData() {
    MR::zeroMemory(_14, 0x10000);
    mNANDRequestInfo->setReadSeq(::cSaveFileName, _14, 0x10000, &_C);
    MR::addRequestToNANDManager(mNANDRequestInfo);

    setNerve(GET_NERVE_ANON(SaveDataHandlerProcessing));
}

bool SaveDataHandler::requestVerifyAfterLoadGameDataFile() {
    if (_C < sizeof(SaveDataFileHeader)) {
        return false;
    }

    SaveDataFileHeader* pFileHeader = reinterpret_cast< SaveDataFileHeader* >(_14);

    if (isCorrectFileHeader(_14)) {
        if (!isValidSaveHeaderFields(pFileHeader->mVersion, pFileHeader->mFileSize, pFileHeader->mUserFileInfoNum, _C)) {
            return false;
        }

        u32 checkSumLE = MR::calcCheckSum(_14 + sizeof(u32), pFileHeader->mFileSize - sizeof(u32));
        if (pFileHeader->mCheckSum != checkSumLE) {
#if defined(TARGET_PC)
            copySaveDataEachFile(_18, _14);
            return true;
#else
            return false;
#endif
        }

        copySaveDataEachFile(_18, _14);
        return true;
    }

    // Handle Big-Endian (Wii / Dolphin) GameData.bin saves by verifying and converting in-place to Little-Endian.
    u32 beCheckSum = readBE32(_14 + 0);
    u32 beVersion = readBE32(_14 + 4);
    u32 beInfoNum = readBE32(_14 + 8);
    u32 beFileSize = readBE32(_14 + 12);

    if (!isValidSaveHeaderFields(beVersion, beFileSize, beInfoNum, _C)) {
        return false;
    }

    u32 checkSumBE = MR::calcCheckSumBE(_14 + sizeof(u32), beFileSize - sizeof(u32));
    if (beCheckSum != checkSumBE) {
#if !defined(TARGET_PC)
        return false;
#endif
    }

    if (!convertGameDataBufferBEtoLE(_14, _C)) {
        return false;
    }

    copySaveDataEachFile(_18, _14);
    return true;
}

void SaveDataHandler::initializeUserFileMemory(int index, const UserFile* pUserFile) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(_18);

    for (u32 i = 0; i < sizeof(::cSaveFileSpecTable) / sizeof(*::cSaveFileSpecTable); i++) {
        char fileName[16];
        snprintf(fileName, sizeof(fileName), "%s%1d", ::cSaveFileSpecTable[i].mName, index);

        SaveDataUserFileInfo userFileInfo;
        fileAccessor.makeUserFileInfo(&userFileInfo, fileName);

        if (userFileInfo.mKind == 0) {
            pUserFile->makeGameDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
        } else if (userFileInfo.mKind == 1) {
            pUserFile->makeConfigDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
        }
    }
}

void SaveDataHandler::copyUserFileMemory(int indexDst, int indexSrc) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(_18);

    for (u32 i = 0; i < sizeof(::cSaveFileSpecTable) / sizeof(*::cSaveFileSpecTable); i++) {
        char srcFileName[16];
        snprintf(srcFileName, sizeof(srcFileName), "%s%1d", ::cSaveFileSpecTable[i].mName, indexSrc);

        SaveDataUserFileInfo srcUserFileInfo;
        fileAccessor.makeUserFileInfo(&srcUserFileInfo, srcFileName);

        char dstFileName[16];
        snprintf(dstFileName, sizeof(dstFileName), "%s%1d", ::cSaveFileSpecTable[i].mName, indexDst);

        SaveDataUserFileInfo dstUserFileInfo;
        fileAccessor.makeUserFileInfo(&dstUserFileInfo, dstFileName);

        MR::copyMemory(dstUserFileInfo.mData, srcUserFileInfo.mData, dstUserFileInfo.mDataSize);
    }
}

void SaveDataHandler::restoreGameDataFile(const char* pUserFileName, void* pParam2, u32 param3) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(_18);

    SaveDataUserFileInfo userFileInfo;
    fileAccessor.makeUserFileInfo(&userFileInfo, pUserFileName);

    MR::copyMemory(pParam2, userFileInfo.mData, userFileInfo.mDataSize);
}

void SaveDataHandler::storeUserFile(const UserFile* pUserFile) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(_18);

    SaveDataUserFileInfo gameFileInfo;
    fileAccessor.makeUserFileInfo(&gameFileInfo, pUserFile->getGameDataName());
    pUserFile->makeGameDataBinary(gameFileInfo.mData, gameFileInfo.mDataSize);

    SaveDataUserFileInfo configFileInfo;
    fileAccessor.makeUserFileInfo(&configFileInfo, pUserFile->getConfigDataName());
    pUserFile->makeConfigDataBinary(configFileInfo.mData, configFileInfo.mDataSize);
}

void SaveDataHandler::storeSysConfigFile(const SysConfigFile* pSysConfigFile) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(_18);

    SaveDataUserFileInfo userFileInfo;
    fileAccessor.makeUserFileInfo(&userFileInfo, ::cSaveFileSpecSystem.mName);
    pSysConfigFile->makeDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
}

void SaveDataHandler::requestSaveSaveData() {
    SaveDataFileAccessor fileAccessorA = SaveDataFileAccessor(_18);
    SaveDataFileAccessor fileAccessorB = SaveDataFileAccessor(_14);

    MR::copyMemory(_14, _18, fileAccessorA.getHeader()->mFileSize);
    MR::fillMemory(_14 + fileAccessorA.getHeader()->mFileSize, 0,
                   OSRoundUp32B(fileAccessorA.getHeader()->mFileSize) - fileAccessorA.getHeader()->mFileSize);
    fileAccessorB.getHeader()->mCheckSum = MR::calcCheckSum(_14 + sizeof(u32), reinterpret_cast< SaveDataFileHeader* >(_14)->mFileSize - sizeof(u32));

    SaveDataFileHeader* pHeader = fileAccessorB.getHeader();
    mNANDRequestInfo->setWriteSeq(::cSaveFileName, _14, OSRoundUp32B(pHeader->mFileSize), 60, 0);
    MR::addRequestToNANDManager(mNANDRequestInfo);

    setNerve(GET_NERVE_ANON(SaveDataHandlerSaveProcessingGameData));
}

void SaveDataHandler::requestRemoveSaveData() {
    setNerve(GET_NERVE_ANON(SaveDataHandlerRemoveProcessingBanner));
}

u32 SaveDataHandler::getEnoughtTempBufferSize() {
    return 0xF80;
}

bool SaveDataHandler::isDone() const {
    bool result = isNerve(GET_NERVE_ANON(SaveDataHandlerWait)) && mNANDRequestInfo->isDone();

    return isNerve(GET_NERVE_ANON(SaveDataHandlerWait));
}

NANDResultCode SaveDataHandler::getLastResultCode() const {
    return NANDResultCode(mNANDRequestInfo->mResult);
}

void SaveDataHandler::exeWait() {
}

void SaveDataHandler::exeProcessing() {
    if (!mNANDRequestInfo->isDone()) {
        return;
    }

    setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
}

void SaveDataHandler::exeSaveProcessingGameData() {
    bool isErr = false;

    if (!trySave(&isErr, mNANDRequestInfo->isDone())) {
        return;
    }

    if (!isErr) {
        setNerve(GET_NERVE_ANON(SaveDataHandlerSaveProcessingBanner));
    } else {
        setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
    }
}

void SaveDataHandler::exeSaveProcessingBanner() {
    if (MR::isFirstStep(this)) {
        mBannerCreator->execute();
    } else if (mBannerCreator->isDone()) {
        NANDResultCode resultCode = mBannerCreator->getResultCode();
        mNANDRequestInfo->mResult = resultCode.getCode();

        if (resultCode.isSuccess()) {
            setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
            return;
        } else {
            setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
        }
    }
}

void SaveDataHandler::exeRemoveProcessingGameData() {
    bool isRemoved = false;

    if (!tryRemoveFile(::cSaveFileName, &isRemoved)) {
        return;
    }

    setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
}

void SaveDataHandler::exeRemoveProcessingBanner() {
    bool isRemoved = false;

    if (!tryRemoveFile(::cBannerFileName, &isRemoved)) {
        return;
    }

    if (isRemoved) {
        setNerve(GET_NERVE_ANON(SaveDataHandlerRemoveProcessingGameData));
    } else {
        setNerve(GET_NERVE_ANON(SaveDataHandlerWait));
    }
}

void SaveDataHandler::resetSaveData(u8* pBuffer) {
    SaveDataFile* pFile = reinterpret_cast< SaveDataFile* >(pBuffer);
    u32 offset = 0x140;

    pFile->mHeader.mCheckSum = 0;
    pFile->mHeader.mVersion = 2;
    pFile->mHeader.mUserFileInfoNum = 19;
    pFile->mHeader.mFileSize = 0;

    SaveDataFileInfo* pFileInfos = pFile->mInfo;
    int fileIndex = 0;

    for (int i = 0; i < 6; i++) {
        for (u32 j = 0; j < sizeof(::cSaveFileSpecTable) / sizeof(*::cSaveFileSpecTable); j++) {
            SaveDataFileInfo* pFileInfo = &pFileInfos[fileIndex];

            MR::zeroMemory(pFileInfo->mName, sizeof(pFileInfo->mName));

            char fileName[32];
            snprintf(fileName, sizeof(fileName), "%s%1d", ::cSaveFileSpecTable[j].mName, i + 1);

            snprintf(pFileInfo->mName, sizeof(pFileInfo->mName), "%s", fileName);
            pFileInfo->mOffset = offset;

            offset += ::cSaveFileSpecTable[j].mBufferSize;
            MR::zeroMemory(pBuffer + pFileInfo->mOffset, ::cSaveFileSpecTable[j].mBufferSize);
            fileIndex++;
        }
    }

    SaveDataFileInfo* pFileInfo = &pFileInfos[fileIndex];

    MR::zeroMemory(pFileInfo->mName, sizeof(pFileInfo->mName));

    snprintf(pFileInfo->mName, sizeof(pFileInfo->mName), "%s", ::cSaveFileSpecSystem.mName);
    pFileInfo->mOffset = offset;

    MR::zeroMemory(pBuffer + pFileInfo->mOffset, ::cSaveFileSpecSystem.mBufferSize);

    pFile->mHeader.mFileSize = offset + ::cSaveFileSpecSystem.mBufferSize;

    u32 padding = OSRoundUp32B(offset + ::cSaveFileSpecSystem.mBufferSize) - pFile->mHeader.mFileSize;
    u8* pPadding = pBuffer + pFile->mHeader.mFileSize;
    for (u32 i = 0; i < padding; i++) {
        pPadding[i] = 0;
    }
}

void SaveDataHandler::initializeAllFileInSaveData(u8* pBuffer, const SysConfigFile* pSysConfigFile, const UserFile* pUserFile) {
    SaveDataFileAccessor fileAccessor = SaveDataFileAccessor(pBuffer);

    SaveDataFileHeader* pHeader = fileAccessor.getHeader();
    SaveDataFileInfo* pFileInfos = reinterpret_cast< SaveDataFile* >(pBuffer)->mInfo;

    for (int i = 0; i < pHeader->mUserFileInfoNum; i++) {
        SaveDataUserFileInfo userFileInfo;
        fileAccessor.makeUserFileInfo(&userFileInfo, pFileInfos[i].mName);

        if (userFileInfo.mKind == 0) {
            pUserFile->makeGameDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
        } else if (userFileInfo.mKind == 1) {
            pUserFile->makeConfigDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
        } else {
            pSysConfigFile->makeDataBinary(userFileInfo.mData, userFileInfo.mDataSize);
        }
    }
}

bool SaveDataHandler::isCorrectFileHeader(const u8* pBuffer) {
    SaveDataFileHeader* pFileHeader = SaveDataFileAccessor(const_cast< u8* >(pBuffer)).getHeader();

    if (pFileHeader->mVersion != 2) {
        return false;
    }

    if (pFileHeader->mFileSize > 0x10000) {
        return false;
    }

    return pFileHeader->mUserFileInfoNum > 0 && pFileHeader->mUserFileInfoNum < 24;
}

void SaveDataHandler::copySaveDataEachFile(u8* pFileDst, const u8* pFileSrc) {
    SaveDataFileAccessor srcFileAccessor = SaveDataFileAccessor(const_cast< u8* >(pFileSrc));
    SaveDataFileAccessor dstFileAccessor = SaveDataFileAccessor(pFileDst);

    int fileNum = srcFileAccessor.getHeader()->mUserFileInfoNum;
    if (fileNum > 24) {
        fileNum = 24;
    }

    for (int i = 0; i < fileNum; i++) {
        const SaveDataFileInfo* pFileInfo = srcFileAccessor.getFileInfo(i);
        if (!pFileInfo) continue;
        const char* pFileName = pFileInfo->mName;
        if (!pFileName || pFileName[0] == '\0') continue;

        SaveDataUserFileInfo srcUserFileInfo;
        srcFileAccessor.makeUserFileInfo(&srcUserFileInfo, pFileName);

        SaveDataUserFileInfo dstUserFileInfo;
        dstFileAccessor.makeUserFileInfo(&dstUserFileInfo, pFileName);

        if (dstUserFileInfo.mData != nullptr && srcUserFileInfo.mData != nullptr) {
            u32 copySize = (dstUserFileInfo.mDataSize < srcUserFileInfo.mDataSize) ? dstUserFileInfo.mDataSize : srcUserFileInfo.mDataSize;
            MR::copyMemory(dstUserFileInfo.mData, srcUserFileInfo.mData, copySize);

            if (dstUserFileInfo.mDataSize > copySize) {
                MR::fillMemory(reinterpret_cast< u8* >(dstUserFileInfo.mData) + copySize, 0, dstUserFileInfo.mDataSize - copySize);
            }
        }
    }
}

void SaveDataHandler::createCommunicationBuffer() {
    _14 = new (MR::getStationedHeapGDDR3(), 32) u8[0x10000];
    _18 = new (MR::getStationedHeapGDDR3(), 32) u8[0x10000];
}

bool SaveDataHandler::tryRemoveFile(const char* pFileName, bool* pIsRemoved) {
    if (MR::isFirstStep(this)) {
        MR::addRequestToNANDManager(mNANDRequestInfo->setDelete(pFileName));
    } else if (mNANDRequestInfo->isDone()) {
        NANDResultCode resultCode = NANDResultCode(mNANDRequestInfo->mResult);

        if (resultCode.isSuccess() || resultCode.isNoExistFile()) {
            *pIsRemoved = true;
            return true;
        } else {
            *pIsRemoved = false;
            return true;
        }
    }

    return false;
}

bool SaveDataHandler::trySave(bool* pIsErr, bool isDone) {
    if (!isDone) {
        return false;
    }

    NANDResultCode resultCode = NANDResultCode(mNANDRequestInfo->mResult);

    if (resultCode.isSuccess()) {
        *pIsErr = false;
        return true;
    } else {
        *pIsErr = true;
        return true;
    }
}
