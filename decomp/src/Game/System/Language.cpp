#include "Game/System/Language.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/MessageHolder.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <revolution/dvd.h>
#include <cstdio>
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/StringUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <revolution/sc.h>

#define REGION_EU 0
#define REGION_JP 1
#define REGION_US 2
#define REGION_CN 3
#define REGION_KR 4
#define REGION_SHIFT 0x4
#define REGION_SHIFT_EU (REGION_EU << REGION_SHIFT)
#define REGION_SHIFT_JP (REGION_JP << REGION_SHIFT)
#define REGION_SHIFT_US (REGION_US << REGION_SHIFT)
#define REGION_SHIFT_CN (REGION_CN << REGION_SHIFT)
#define REGION_SHIFT_KR (REGION_KR << REGION_SHIFT)
#define REGION_MASK (0xF << REGION_SHIFT)

#define LANGUAGE_JPJAPANESE (REGION_SHIFT_JP | SC_LANG_JAPANESE)
#define LANGUAGE_USENGLISH (REGION_SHIFT_US | SC_LANG_ENGLISH)
#define LANGUAGE_USSPANISH (REGION_SHIFT_US | SC_LANG_SPANISH)
#define LANGUAGE_USFRENCH (REGION_SHIFT_US | SC_LANG_FRENCH)
#define LANGUAGE_EUENGLISH (REGION_SHIFT_EU | SC_LANG_ENGLISH)
#define LANGUAGE_EUSPANISH (REGION_SHIFT_EU | SC_LANG_SPANISH)
#define LANGUAGE_EUFRENCH (REGION_SHIFT_EU | SC_LANG_FRENCH)
#define LANGUAGE_EUGERMAN (REGION_SHIFT_EU | SC_LANG_GERMAN)
#define LANGUAGE_EUITALIAN (REGION_SHIFT_EU | SC_LANG_ITALIAN)
#define LANGUAGE_EUDUTCH (REGION_SHIFT_EU | SC_LANG_DUTCH)
#define LANGUAGE_CNSIMPCHINESE (REGION_SHIFT_CN | SC_LANG_SIMP_CHINESE)
#define LANGUAGE_KRKOREAN (REGION_SHIFT_KR | SC_LANG_KOREAN)
#define LANGUAGE_MASK (~REGION_MASK)

namespace {
    const u8 cSCLanguage2GameLanguageTable[][10] = {
        {
            // RMGJ
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            // RMGE
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_USFRENCH,
            LANGUAGE_USSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            // RMGP
            LANGUAGE_JPJAPANESE,
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            // ???
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            // RMGK
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            // ???
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
    };
    const Language cLanguages[] = {
        {LANGUAGE_JPJAPANESE, "JpJapanese"}, {LANGUAGE_USENGLISH, "UsEnglish"},         {LANGUAGE_USSPANISH, "UsSpanish"},
        {LANGUAGE_USFRENCH, "UsFrench"},     {LANGUAGE_EUENGLISH, "EuEnglish"},         {LANGUAGE_EUSPANISH, "EuSpanish"},
        {LANGUAGE_EUFRENCH, "EuFrench"},     {LANGUAGE_EUGERMAN, "EuGerman"},           {LANGUAGE_EUITALIAN, "EuItalian"},
        {LANGUAGE_EUDUTCH, "EuDutch"},       {LANGUAGE_CNSIMPCHINESE, "CnSimpChinese"}, {LANGUAGE_KRKOREAN, "KrKorean"},
    };
};  // namespace

namespace MR {
    u32 getDecidedLanguageFromIPL() {
        s32 language = SCGetLanguage();
        s32 i;

        if (language < 0) {
            i = 0;
        } else {
            s32 size = ARRAY_SIZE(::cSCLanguage2GameLanguageTable[4]);

            if (language <= size) {
                i = language;
            } else {
                i = size;
            }
        }

#ifdef TARGET_PC
        // The decompiled executable is the Korean build (row 4); pick the row
        // of the disc the data comes from (game code "RMGx" at 0x80000000).
        s32 row = 4;
        switch (reinterpret_cast< const char* >(OSPhysicalToCached(0))[3]) {
        case 'J':
            row = 0;
            break;
        case 'E':
            row = 1;
            break;
        case 'P':
            row = 2;
            break;
        default:
            break;
        }
        return ::cSCLanguage2GameLanguageTable[row][i];
#else
        return ::cSCLanguage2GameLanguageTable[4][i];
#endif
    }

    u32 getLanguage() {
        return SingletonHolder< GameSystem >::get()->mObjHolder->mLanguage;
    }

    u32 getLanguageFromIPL() {
        return getLanguage() & LANGUAGE_MASK;
    }

    const char* getCurrentLanguagePrefix() {
        u32 id = getLanguage();

        for (int i = 0; i < getLanguageNum(); i++) {
            if (id == ::cLanguages[i].mId) {
                return ::cLanguages[i].mName;
            }
        }

        return "English";
    }

    const char* getCurrentRegionPrefix() {
#ifdef TARGET_PC
        char prefix[3];  // extractString writes a terminator after the 2 characters
        MR::extractString(prefix, getCurrentLanguagePrefix(), 2, 3);
#else
        char prefix[2];
        MR::extractString(prefix, getCurrentLanguagePrefix(), sizeof(prefix), 3);
#endif

        if (MR::isEqualString(prefix, "Jp")) {
            return "Jp";
        }

        if (MR::isEqualString(prefix, "Us")) {
            return "Us";
        }

        if (MR::isEqualString(prefix, "Eu")) {
            return "Eu";
        }

        if (MR::isEqualString(prefix, "Cn")) {
            return "Cn";
        }

        if (MR::isEqualString(prefix, "Kr")) {
            return "Kr";
        }

        return nullptr;
    }

    u32 getLanguageNum() {
        return ARRAY_SIZE(::cLanguages);
    }

    const char* getLanguagePrefixByIndex(u32 index) {
        return ::cLanguages[index].mName;
    }

    void tryApplyRuntimeLanguageChange() {
#ifdef TARGET_PC
        int langIndex = -1;
        if (!port_take_language_change_request(&langIndex)) {
            return;
        }

        GameSystem* pSys = SingletonHolder< GameSystem >::get();
        if (pSys == nullptr || pSys->mObjHolder == nullptr) {
            return;
        }

        u32 cand1 = LANGUAGE_USENGLISH;
        u32 cand2 = LANGUAGE_EUENGLISH;
        switch (langIndex) {
        case 0: cand1 = LANGUAGE_USENGLISH; cand2 = LANGUAGE_EUENGLISH; break;
        case 1: cand1 = LANGUAGE_USSPANISH; cand2 = LANGUAGE_EUSPANISH; break;
        case 2: cand1 = LANGUAGE_USFRENCH;  cand2 = LANGUAGE_EUFRENCH;  break;
        case 3: cand1 = LANGUAGE_EUGERMAN;  cand2 = LANGUAGE_USENGLISH; break;
        case 4: cand1 = LANGUAGE_EUITALIAN; cand2 = LANGUAGE_USENGLISH; break;
        case 5: cand1 = LANGUAGE_EUDUTCH;   cand2 = LANGUAGE_EUENGLISH; break;
        case 6: cand1 = LANGUAGE_JPJAPANESE; cand2 = LANGUAGE_USENGLISH; break;
        default: break;
        }

        char discRegion = reinterpret_cast< const char* >(OSPhysicalToCached(0))[3];
        if (discRegion == 'P' && (cand2 & REGION_MASK) == REGION_SHIFT_EU) {
            u32 tmp = cand1;
            cand1 = cand2;
            cand2 = tmp;
        }

        u32 chosenId = 0;
        u32 candidates[2] = {cand1, cand2};
        for (int c = 0; c < 2; c++) {
            u32 cid = candidates[c];
            const char* prefix = nullptr;
            for (u32 i = 0; i < getLanguageNum(); i++) {
                if (::cLanguages[i].mId == cid) {
                    prefix = ::cLanguages[i].mName;
                    break;
                }
            }
            if (prefix != nullptr) {
                char testPath[128];
                snprintf(testPath, sizeof(testPath), "/%s/MessageData/Message.arc", prefix);
                if (DVDConvertPathToEntrynum(testPath) >= 0) {
                    chosenId = cid;
                    break;
                }
            }
        }

        if (chosenId == 0 || chosenId == pSys->mObjHolder->mLanguage) {
            return;
        }

        pSys->mObjHolder->mLanguage = chosenId;

        JKRHeap* stationedHeap = MR::getStationedHeapNapa();
        if (stationedHeap == nullptr) {
            return;
        }

        MR::CurrentHeapRestorer heapRestorer(stationedHeap);
        if (!MR::isMountedArchive("/MessageData/Message.arc")) {
            MR::mountArchive("/MessageData/Message.arc", stationedHeap);
        }

        if (MR::isMountedArchive("/MessageData/Message.arc") && pSys->mObjHolder->mMessageHolder != nullptr) {
            MessageData* newMsg = new MessageData("/MessageData/Message.arc");
            bool hadSceneMsg = (pSys->mObjHolder->mMessageHolder->mSceneMessageData != nullptr);
            pSys->mObjHolder->mMessageHolder->mGameMessageData = newMsg;
            if (hadSceneMsg) {
                pSys->mObjHolder->mMessageHolder->mSceneMessageData = newMsg;
            }
            port_log("language: hot-reloaded Message.arc for %s (id=0x%02x)", getCurrentLanguagePrefix(), chosenId);
        }
#endif
    }
};  // namespace MR
