// DVD file system over the cooked game data directory.
//
// Entry numbers come from the disc's own fst.bin, so they match the original
// game exactly; file sizes come from the cooked (decompressed, byte-swapped)
// files on the host.  Reads run on a worker thread and complete through the
// DVD interrupt, like the real drive.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/dvd.h"
#include "revolution/os.h"
#include "revolution/sc.h"

enum {
    kStateEnd = 0,
    kStateBusy = 1,
    kStateWaiting = 2,
    kStateCanceled = 10,
    kStateFatal = -1,
};

struct FstEntry {
    bool isDir;
    std::string name;
    u32 parent;  // dirs: parent entry
    u32 next;    // dirs: index after the last child
    std::string hostPath;
    std::string hdPath;
    std::string modPath;
    std::string activePath;
    s64 size = -1;
    s64 hdSize = -1;
    s64 modSize = -1;
    int fd = -1;
    u64 lastUseTick = 0;
};

static std::vector<FstEntry> sFst;
static std::string sDataRoot;
static std::string sModsRoot;
static u32 sCurrentDir = 0;

struct InstalledMod {
    std::string id;
    std::string name;
    std::string saveId;
    bool cloneSave = true;
    bool enabled = false;
    bool activeAtBoot = false;
    int fileCount = 0;
    int createdCount = 0;
    int ppcPatchCount = 0;
};

static std::mutex sModsMutex;
static std::vector<InstalledMod> sConfiguredMods;
static std::vector<InstalledMod> sActiveBootMods;
static bool sModsBootSnapshotted = false;

static std::string trimStr(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static InstalledMod readModIni(const std::string& modsDir, const std::string& modId, bool enabled) {
    InstalledMod m;
    m.id = modId;
    m.name = modId;
    m.enabled = enabled;
    FILE* f = fopen((modsDir + "/" + modId + "/mod.ini").c_str(), "rb");
    if (!f) return m;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trimStr(line);
        size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = trimStr(s.substr(0, eq));
        std::string v = trimStr(s.substr(eq + 1));
        if (k == "name" && !v.empty()) m.name = v;
        else if (k == "save_id") m.saveId = v;
        else if (k == "clone_save") m.cloneSave = (v != "0" && strcasecmp(v.c_str(), "false") != 0);
        else if (k == "file_count") m.fileCount = atoi(v.c_str());
        else if (k == "created_count") m.createdCount = atoi(v.c_str());
        else if (k == "ppc_patches") m.ppcPatchCount = atoi(v.c_str());
    }
    fclose(f);
    return m;
}

static void reloadModsLocked() {
    std::string root = !sModsRoot.empty() ? sModsRoot : sDataRoot;
    sConfiguredMods.clear();
    if (root.empty()) return;
    std::string modsDir = root + "/mods";
    std::string cfgPath = modsDir + "/mods_config.ini";

    auto wasActiveAtBoot = [](const std::string& id) -> bool {
        for (const auto& b : sActiveBootMods) {
            if (b.id == id) return true;
        }
        return false;
    };

    std::vector<std::string> seen;
    FILE* f = fopen(cfgPath.c_str(), "rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string s = trimStr(line);
            if (s.empty() || s[0] == '#') continue;
            size_t bar = s.find('|');
            if (bar == std::string::npos || bar == 0) continue;
            std::string state = trimStr(s.substr(0, bar));
            std::string modId = trimStr(s.substr(bar + 1));
            if (modId.empty() || !dirExists(modsDir + "/" + modId)) continue;
            bool dup = false;
            for (const auto& x : seen) {
                if (x == modId) { dup = true; break; }
            }
            if (dup) continue;
            seen.push_back(modId);
            InstalledMod m = readModIni(modsDir, modId, strcasecmp(state.c_str(), "enabled") == 0);
            m.activeAtBoot = wasActiveAtBoot(modId);
            sConfiguredMods.push_back(std::move(m));
        }
        fclose(f);
    }

    DIR* dir = opendir(modsDir.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string modId = ent->d_name;
            bool known = false;
            for (const auto& x : seen) {
                if (x == modId) { known = true; break; }
            }
            if (known) continue;
            std::string modPath = modsDir + "/" + modId;
            if (!dirExists(modPath)) continue;
            if (!dirExists(modPath + "/files") && !dirExists(modPath + "/sys")) continue;
            seen.push_back(modId);
            InstalledMod m = readModIni(modsDir, modId, false);
            m.activeAtBoot = wasActiveAtBoot(modId);
            sConfiguredMods.push_back(std::move(m));
        }
        closedir(dir);
    }
}

static bool writeModsConfigLocked() {
    std::string root = !sModsRoot.empty() ? sModsRoot : sDataRoot;
    if (root.empty()) return false;
    std::string modsDir = root + "/mods";
    mkdir(modsDir.c_str(), 0755);
    std::string cfgPath = modsDir + "/mods_config.ini";
    FILE* f = fopen(cfgPath.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "# GalaxyMobile Riivolution Mods Config\n");
    fprintf(f, "# Order from top to bottom = highest to lowest priority\n");
    for (const auto& m : sConfiguredMods) {
        fprintf(f, "%s|%s\n", m.enabled ? "enabled" : "disabled", m.id.c_str());
    }
    fclose(f);
    return true;
}

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// The disc's file table (sys/fst.bin under `root`): 12-byte entries, the
// root's holding their count, then the names.  Empty when missing or cut.
static std::vector<u8> readFst(const std::string& root) {
    std::vector<u8> data;
    FILE* f = fopen((root + "/sys/fst.bin").c_str(), "rb");
    if (!f) {
        return data;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len >= 12) {
        data.resize((size_t)len);
        if (fread(data.data(), 1, data.size(), f) != data.size() || (uint64_t)be32(&data[8]) * 12 >= data.size()) {
            data.clear();
        }
    }
    fclose(f);
    return data;
}

// Whether the disc's root directory has a folder `name`.
static bool fstHasRootDir(const std::vector<u8>& fst, const char* name) {
    u32 count = be32(&fst[8]);
    const char* strings = (const char*)&fst[count * 12];
    size_t stringsSize = fst.size() - count * 12;
    size_t len = strlen(name);
    for (u32 i = 1; i < count;) {
        const u8* e = &fst[i * 12];
        bool isDir = e[0] != 0;
        u32 nameOff = be32(e) & 0x00FFFFFF;
        if (isDir && nameOff + len < stringsSize && strncasecmp(strings + nameOff, name, len + 1) == 0) {
            return true;
        }
        u32 next = be32(e + 8);
        i = (isDir && next > i) ? next : i + 1;  // past a folder's contents
    }
    return false;
}

// Each region's disc keeps its texts and translated layouts in language
// folders named after it (EuEnglish, UsEnglish...), and the game picks the
// folder from the disc's game code and the console's language
// (Language.cpp).  The port tells the disc by those folders in its file
// table, which every conversion has, and runs the game in the language of
// the `language` setting, or the disc's first where it has no such texts.
// (The European disc also has an EuDutch folder: its texts are the English
// ones.)
static const PortDisc kDiscs[] = {
    {"RMGP01",
     "Europe",
     {{"english", "EuEnglish", SC_LANG_ENGLISH},
      {"french", "EuFrench", SC_LANG_FRENCH},
      {"german", "EuGerman", SC_LANG_GERMAN},
      {"spanish", "EuSpanish", SC_LANG_SPANISH},
      {"italian", "EuItalian", SC_LANG_ITALIAN},
      {"dutch", "EuDutch", SC_LANG_DUTCH}},
     6},
    {"RMGE01",
     "North America",
     {{"english", "UsEnglish", SC_LANG_ENGLISH}, {"french", "UsFrench", SC_LANG_FRENCH}, {"spanish", "UsSpanish", SC_LANG_SPANISH}},
     3},
    // Not tried: no such disc at hand.
    {"RMGJ01", "Japan", {{"japanese", "JpJapanese", SC_LANG_JAPANESE}}, 1},
    {"RMGK01", "Korea", {{"korean", "KrKorean", SC_LANG_KOREAN}}, 1},
};
static const PortDisc* sDisc = nullptr;
// The running disc's languages whose texts are in the copy (the first is the
// disc's own, which port_dvd_identify checked), the one asked for
// (port_language_set) and the one the game started in.
static const PortLanguage* sLanguages[PORT_DISC_MAX_LANGUAGES];
static int sLanguageCount = 0;
static char sLanguageWanted[16];
static const PortLanguage* sLanguageRunning = nullptr;

// Whether the copy in `root` has the texts of a language of its disc.
static bool hasTexts(const std::string& root, const std::vector<u8>& fst, const PortLanguage& language) {
    std::string texts = root + "/files/" + language.folder + "/MessageData/Message.arc";
    struct stat st;
    return fstHasRootDir(fst, language.folder) && stat(texts.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

extern "C" const PortDisc* port_dvd_identify(const char* root) {
    PortHostAllocScope scope;
    std::vector<u8> fst = readFst(root);
    if (fst.empty()) {
        return nullptr;
    }
    for (const PortDisc& disc : kDiscs) {
        // The disc has the folder of its first language, and the copy the
        // texts in it.
        if (hasTexts(root, fst, disc.languages[0])) {
            return &disc;
        }
    }
    return nullptr;
}

extern "C" const PortDisc* port_dvd_disc(void) { return sDisc; }

extern "C" void port_dvd_set_root(const char* root) {
    PortHostAllocScope scope;
    sDataRoot = root;
    {
        std::lock_guard<std::mutex> lk(sModsMutex);
        if (root && root[0]) sModsRoot = root;
        reloadModsLocked();
        sActiveBootMods.clear();
        for (auto& m : sConfiguredMods) {
            if (m.enabled) {
                m.activeAtBoot = true;
                sActiveBootMods.push_back(m);
            }
        }
        sModsBootSnapshotted = true;
        if (!sActiveBootMods.empty()) {
            port_log("mods: %zu Riivolution mod(s) enabled for boot", sActiveBootMods.size());
        }
    }
    sDisc = port_dvd_identify(root);
    sLanguageCount = 0;
    if (sDisc) {
        std::vector<u8> fst = readFst(root);
        for (int i = 0; i < sDisc->languageCount; i++) {
            if (i == 0 || hasTexts(root, fst, sDisc->languages[i])) {
                sLanguages[sLanguageCount++] = &sDisc->languages[i];
            }
        }
    }
    // Debug: PETARI_LANGUAGE=<name> instead of the setting.
    if (const char* name = getenv("PETARI_LANGUAGE")) {
        port_language_set(name);
    }
}

extern "C" void port_mods_set_root(const char* dataRoot) {
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sModsMutex);
    if (dataRoot && dataRoot[0]) {
        sModsRoot = dataRoot;
    }
    reloadModsLocked();
}

extern "C" void port_mods_reload(void) {
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sModsMutex);
    reloadModsLocked();
}

extern "C" int port_mods_count(void) {
    std::lock_guard<std::mutex> lk(sModsMutex);
    return (int)sConfiguredMods.size();
}

extern "C" int port_mods_get(int index, PortModInfo* outInfo) {
    std::lock_guard<std::mutex> lk(sModsMutex);
    if (!outInfo || index < 0 || index >= (int)sConfiguredMods.size()) return 0;
    const InstalledMod& m = sConfiguredMods[index];
    memset(outInfo, 0, sizeof(*outInfo));
    snprintf(outInfo->id, sizeof(outInfo->id), "%s", m.id.c_str());
    snprintf(outInfo->name, sizeof(outInfo->name), "%s", m.name.c_str());
    snprintf(outInfo->saveId, sizeof(outInfo->saveId), "%s", m.saveId.c_str());
    outInfo->enabled = m.enabled ? 1 : 0;
    outInfo->activeAtBoot = m.activeAtBoot ? 1 : 0;
    outInfo->fileCount = m.fileCount;
    outInfo->createdCount = m.createdCount;
    outInfo->ppcPatchCount = m.ppcPatchCount;
    return 1;
}

extern "C" int port_mods_set_enabled(const char* modId, int enabled) {
    PortHostAllocScope scope;
    if (!modId || !modId[0]) return 0;
    std::lock_guard<std::mutex> lk(sModsMutex);
    for (auto& m : sConfiguredMods) {
        if (m.id == modId) {
            m.enabled = (enabled != 0);
            return writeModsConfigLocked() ? 1 : 0;
        }
    }
    return 0;
}

extern "C" int port_mods_has_pending_changes(void) {
    std::lock_guard<std::mutex> lk(sModsMutex);
    if (!sModsBootSnapshotted) return 0;
    for (const auto& m : sConfiguredMods) {
        if (m.enabled != m.activeAtBoot) return 1;
    }
    return 0;
}

extern "C" int port_mods_get_active_save(char* outSaveId, size_t maxLen, int* outCloneVanilla) {
    std::lock_guard<std::mutex> lk(sModsMutex);
    for (const auto& m : sActiveBootMods) {
        if (!m.saveId.empty()) {
            if (outSaveId && maxLen > 0) snprintf(outSaveId, maxLen, "%s", m.saveId.c_str());
            if (outCloneVanilla) *outCloneVanilla = m.cloneSave ? 1 : 0;
            return 1;
        }
    }
    if (outSaveId && maxLen > 0) outSaveId[0] = 0;
    return 0;
}

extern "C" int port_dvd_resolve_sys_file(const char* name, char* outPath, size_t maxLen) {
    PortHostAllocScope scope;
    if (!name || !name[0] || !outPath || maxLen == 0) return 0;
    outPath[0] = 0;
    std::lock_guard<std::mutex> lk(sModsMutex);
    std::string root = !sModsRoot.empty() ? sModsRoot : sDataRoot;
    if (root.empty()) return 0;
    for (const auto& m : sActiveBootMods) {
        std::string cand = root + "/mods/" + m.id + "/sys/" + name;
        if (fileExists(cand)) {
            snprintf(outPath, maxLen, "%s", cand.c_str());
            return 1;
        }
    }
    return 0;
}

extern "C" int port_language_count(void) { return sLanguageCount; }

extern "C" const char* port_language_name(int index) { return index >= 0 && index < sLanguageCount ? sLanguages[index]->name : ""; }

extern "C" void port_language_set(const char* name) {
    size_t n = 0;
    for (; name && name[n] && n + 1 < sizeof(sLanguageWanted); n++) {
        sLanguageWanted[n] = (char)tolower((unsigned char)name[n]);
    }
    sLanguageWanted[n] = 0;
}

static int sGameLanguageIndex = 0;
static int sPendingLangChange = -1;

static const char* kIndexToLangName[7] = {
    "english",
    "spanish",
    "french",
    "german",
    "italian",
    "dutch",
    "japanese"
};

extern "C" void port_set_game_language(int index) {
    if (index < 0 || index >= 7) index = 0;
    sGameLanguageIndex = index;
    port_language_set(kIndexToLangName[index]);
    sPendingLangChange = index;
}

extern "C" int port_get_game_language(void) {
    return sGameLanguageIndex;
}

extern "C" int port_take_language_change_request(int* outLangIndex) {
    int req = sPendingLangChange;
    if (req >= 0) {
        sPendingLangChange = -1;
        if (outLangIndex) *outLangIndex = req;
        return 1;
    }
    return 0;
}

static std::mutex sUiSeMu;
static int sUiSeBuf[16] = {};
static int sUiSeHead = 0;
static int sUiSeTail = 0;

extern "C" void port_play_ui_se(int seType) {
    if (seType <= 0) return;
    std::lock_guard<std::mutex> lock(sUiSeMu);
    int nextTail = (sUiSeTail + 1) & 15;
    if (nextTail == sUiSeHead) {
        sUiSeHead = (sUiSeHead + 1) & 15;
    }
    sUiSeBuf[sUiSeTail] = seType;
    sUiSeTail = nextTail;
}

extern "C" int port_take_ui_se_request(void) {
    std::lock_guard<std::mutex> lock(sUiSeMu);
    if (sUiSeHead == sUiSeTail) return 0;
    int req = sUiSeBuf[sUiSeHead];
    sUiSeHead = (sUiSeHead + 1) & 15;
    return req;
}

extern "C" int port_language_wanted(void) {
    for (int i = 0; i < sLanguageCount; i++) {
        if (strcmp(sLanguageWanted, sLanguages[i]->name) == 0) {
            return i;
        }
    }
    return 0;
}

extern "C" int port_language_running(void) {
    for (int i = 0; i < sLanguageCount; i++) {
        if (sLanguages[i] == sLanguageRunning) {
            return i;
        }
    }
    return -1;
}

extern "C" int port_language_start(void) {
    if (sLanguageCount == 0) {
        return SC_LANG_ENGLISH;
    }
    const PortLanguage* language = sLanguages[port_language_wanted()];
    if (language != sLanguageRunning) {
        sLanguageRunning = language;
        port_log("language: %s (texts from %s)", language->name, language->folder);
    }
    return language->code;
}

extern "C" const char* port_dvd_root(void) { return sDataRoot.c_str(); }

struct FstTreeNode {
    bool isDir = false;
    std::string name;
    std::string hostPath;
    std::string hdPath;
    std::string modPath;
    s64 hdSize = -1;
    s64 modSize = -1;
    std::vector<std::unique_ptr<FstTreeNode>> children;
};

static FstTreeNode* findChildNode(FstTreeNode* parent, const char* name) {
    for (const auto& ch : parent->children) {
        if (strcasecmp(ch->name.c_str(), name) == 0) {
            return ch.get();
        }
    }
    return nullptr;
}

static void overlayModDirectory(FstTreeNode* parentNode, const std::string& hostDirPath,
                                int& replacedCount, int& createdCount) {
    DIR* dir = opendir(hostDirPath.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string childHost = hostDirPath + "/" + ent->d_name;
        struct stat st;
        if (stat(childHost.c_str(), &st) != 0) continue;
        FstTreeNode* match = findChildNode(parentNode, ent->d_name);
        if (S_ISDIR(st.st_mode)) {
            if (!match) {
                auto node = std::make_unique<FstTreeNode>();
                node->isDir = true;
                node->name = ent->d_name;
                match = node.get();
                parentNode->children.push_back(std::move(node));
            }
            if (match->isDir) {
                overlayModDirectory(match, childHost, replacedCount, createdCount);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (match && !match->isDir) {
                if (match->modPath.empty()) {
                    match->modPath = childHost;
                    match->modSize = (s64)st.st_size;
                    replacedCount++;
                }
            } else if (!match) {
                auto node = std::make_unique<FstTreeNode>();
                node->isDir = false;
                node->name = ent->d_name;
                node->hostPath = childHost;
                node->modPath = childHost;
                node->modSize = (s64)st.st_size;
                parentNode->children.push_back(std::move(node));
                createdCount++;
            }
        }
    }
    closedir(dir);
}

static void flattenFstTree(const FstTreeNode* node, u32 parentIdx, std::vector<FstEntry>& out) {
    u32 myIdx = (u32)out.size();
    out.emplace_back();
    out[myIdx].isDir = node->isDir;
    out[myIdx].name = node->name;
    out[myIdx].parent = parentIdx;
    out[myIdx].hostPath = node->hostPath;
    out[myIdx].hdPath = node->hdPath;
    out[myIdx].modPath = node->modPath;
    out[myIdx].activePath = !node->modPath.empty() ? node->modPath : node->hostPath;
    out[myIdx].hdSize = node->hdSize;
    out[myIdx].modSize = node->modSize;
    if (node->isDir) {
        for (const auto& ch : node->children) {
            flattenFstTree(ch.get(), myIdx, out);
        }
        out[myIdx].next = (u32)out.size();
    } else {
        out[myIdx].next = myIdx + 1;
    }
}

static void applyBootModsToFst() {
    std::vector<InstalledMod> bootMods;
    std::string root;
    {
        std::lock_guard<std::mutex> lk(sModsMutex);
        bootMods = sActiveBootMods;
        root = !sModsRoot.empty() ? sModsRoot : sDataRoot;
    }
    if (bootMods.empty() || sFst.empty()) return;

    auto treeRoot = std::make_unique<FstTreeNode>();
    treeRoot->isDir = true;
    treeRoot->name = sFst[0].name;

    std::vector<std::pair<u32, FstTreeNode*>> stack;
    stack.push_back({sFst[0].next, treeRoot.get()});
    for (u32 i = 1; i < (u32)sFst.size(); i++) {
        while (stack.size() > 1 && i >= stack.back().first) {
            stack.pop_back();
        }
        const FstEntry& fe = sFst[i];
        auto node = std::make_unique<FstTreeNode>();
        node->isDir = fe.isDir;
        node->name = fe.name;
        node->hostPath = fe.hostPath;
        node->hdPath = fe.hdPath;
        node->hdSize = fe.hdSize;
        FstTreeNode* raw = node.get();
        stack.back().second->children.push_back(std::move(node));
        if (fe.isDir) {
            stack.push_back({fe.next, raw});
        }
    }

    int totalReplaced = 0;
    int totalCreated = 0;
    for (const auto& mod : bootMods) {
        std::string modFiles = root + "/mods/" + mod.id + "/files";
        if (!dirExists(modFiles)) continue;
        int rep = 0, cre = 0;
        overlayModDirectory(treeRoot.get(), modFiles, rep, cre);
        totalReplaced += rep;
        totalCreated += cre;
        port_log("mods: [Riivolution] mounted '%s' (%d replaced, %d created)", mod.name.c_str(), rep, cre);
    }

    if (totalReplaced > 0 || totalCreated > 0) {
        std::vector<FstEntry> rebuilt;
        rebuilt.reserve(sFst.size() + (size_t)totalCreated + 32);
        flattenFstTree(treeRoot.get(), 0, rebuilt);
        sFst.swap(rebuilt);
        port_log("mods: FST overlay rebuilt -> %zu entries (%d replaced, %d new)",
                 sFst.size(), totalReplaced, totalCreated);
    }
}

static bool loadFst() {
    PortHostAllocScope scope;
    std::vector<u8> data = readFst(sDataRoot);
    if (data.empty()) {
        port_log("dvd: cannot read %s/sys/fst.bin", sDataRoot.c_str());
        return false;
    }

    u32 count = be32(&data[8]);
    const u8* strings = &data[count * 12];
    sFst.resize(count);
    // Directory path of each directory entry, built as we walk.
    std::vector<std::string> dirPath(count);
    dirPath[0] = "";
    std::vector<u32> stack;  // (dir index) while walking
    stack.push_back(0);
    sFst[0].isDir = true;
    sFst[0].parent = 0;
    sFst[0].next = count;
    for (u32 i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= sFst[stack.back()].next) {
            stack.pop_back();
        }
        const u8* e = &data[i * 12];
        FstEntry& fe = sFst[i];
        fe.isDir = e[0] != 0;
        u32 nameOff = be32(e) & 0x00FFFFFF;
        fe.name = (const char*)(strings + nameOff);
        u32 parentDir = stack.back();
        std::string full = dirPath[parentDir].empty() ? fe.name : dirPath[parentDir] + "/" + fe.name;
        if (fe.isDir) {
            fe.parent = be32(e + 4);
            fe.next = be32(e + 8);
            dirPath[i] = full;
            stack.push_back(i);
        } else {
            fe.hostPath = sDataRoot + "/files/" + full;
            fe.activePath = fe.hostPath;
            std::string hd = sDataRoot + "/plus_hd/" + full;
            struct stat st;
            if (stat(hd.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                fe.hdPath = hd;
                fe.hdSize = (s64)st.st_size;
            }
        }
    }
    applyBootModsToFst();
    port_log("dvd: fst loaded, %zu entries, root %s", sFst.size(), sDataRoot.c_str());
    return true;
}

static std::mutex sFstMutex;

static s64 entrySize(u32 entry) {
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sFstMutex);
    FstEntry& fe = sFst[entry];
    if (!fe.modPath.empty()) {
        return fe.modSize;
    }
    if (port_is_hd_ui_enabled() && !fe.hdPath.empty()) {
        return fe.hdSize;
    }
    if (fe.size < 0) {
        struct stat st;
        fe.size = (stat(fe.hostPath.c_str(), &st) == 0) ? (s64)st.st_size : 0;
        if (fe.size == 0) {
            port_log("dvd: missing file %s", fe.hostPath.c_str());
        }
    }
    return fe.size;
}

// Case-insensitive lookup of `name` among the children of directory `dir`.
static s32 findChild(u32 dir, const char* name, size_t len) {
    for (u32 i = dir + 1; i < sFst[dir].next;) {
        const FstEntry& fe = sFst[i];
        if (fe.name.size() == len && strncasecmp(fe.name.c_str(), name, len) == 0) {
            return (s32)i;
        }
        i = fe.isDir ? fe.next : i + 1;
    }
    return -1;
}

extern "C" s32 DVDConvertPathToEntrynum(const char* path) {
    u32 dir = (path[0] == '/') ? 0 : sCurrentDir;
    const char* p = path;
    while (*p == '/') {
        p++;
    }
    while (*p) {
        const char* end = p;
        while (*end && *end != '/') {
            end++;
        }
        size_t len = (size_t)(end - p);
        s32 found;
        if (len == 1 && p[0] == '.') {
            found = (s32)dir;
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            found = (s32)sFst[dir].parent;
        } else {
            found = findChild(dir, p, len);
        }
        if (found < 0) {
            return -1;
        }
        if (*end == 0) {
            return found;
        }
        if (!sFst[found].isDir) {
            return -1;
        }
        dir = (u32)found;
        p = end + 1;
        while (*p == '/') {
            p++;
        }
    }
    return (s32)dir;
}

// ---------------------------------------------------------------------------
// Read worker
// ---------------------------------------------------------------------------
struct ReadRequest {
    DVDCommandBlock* block;
    u32 entry;
};

static std::mutex sQueueMutex;
static std::condition_variable sQueueCv;
static std::deque<ReadRequest> sPending;       // not yet started
static std::deque<DVDCommandBlock*> sDone;     // finished, callbacks pending
static DVDCommandBlock* sActive = nullptr;     // being read by the worker
static OSThreadQueue sSyncQueue;

static constexpr int kMaxOpenFds = 64;
static int sOpenFdCount = 0;
static u64 sFdAccessSeq = 0;

static int openEntry(u32 entry) {
    PortHostAllocScope scope;
    std::lock_guard<std::mutex> lk(sFstMutex);
    FstEntry& fe = sFst[entry];
    const std::string& neededPath =
        !fe.modPath.empty() ? fe.modPath
                            : ((port_is_hd_ui_enabled() && !fe.hdPath.empty()) ? fe.hdPath : fe.hostPath);
    if (fe.fd >= 0 && fe.activePath != neededPath) {
        close(fe.fd);
        fe.fd = -1;
        if (sOpenFdCount > 0) sOpenFdCount--;
    }
    if (fe.fd < 0) {
        while (sOpenFdCount >= kMaxOpenFds) {
            int lruIdx = -1;
            u64 minTick = ~0ull;
            for (size_t i = 0; i < sFst.size(); i++) {
                if (sFst[i].fd >= 0 && i != entry && sFst[i].lastUseTick < minTick) {
                    minTick = sFst[i].lastUseTick;
                    lruIdx = (int)i;
                }
            }
            if (lruIdx < 0) break;
            close(sFst[lruIdx].fd);
            sFst[lruIdx].fd = -1;
            if (sOpenFdCount > 0) sOpenFdCount--;
        }
        fe.activePath = neededPath;
        fe.fd = open(neededPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fe.fd < 0) {
            port_log("dvd: open %s failed: %s", neededPath.c_str(), strerror(errno));
        } else {
            sOpenFdCount++;
            fe.lastUseTick = ++sFdAccessSeq;
            if (!fe.modPath.empty() && neededPath == fe.modPath) {
                port_log("dvd: [Riivolution] loaded %s", neededPath.c_str());
            } else if (!fe.hdPath.empty() && neededPath == fe.hdPath) {
                port_log("dvd: [Plus HD] loaded %s", neededPath.c_str());
            }
        }
    } else {
        fe.lastUseTick = ++sFdAccessSeq;
    }
    return fe.fd;
}

static void workerMain() {
    for (;;) {
        ReadRequest req;
        {
            std::unique_lock<std::mutex> lk(sQueueMutex);
            sQueueCv.wait(lk, [] { return !sPending.empty(); });
            req = sPending.front();
            sPending.pop_front();
            sActive = req.block;
        }
        DVDCommandBlock* b = req.block;
        s32 result;
        int fd = openEntry(req.entry);
        if (fd < 0) {
            result = -1;
        } else {
            u8* dst = (u8*)b->addr;
            size_t want = b->length;
            size_t totalGot = 0;
            bool ioErr = false;
            while (totalGot < want) {
                ssize_t got = pread(fd, dst + totalGot, want - totalGot, (off_t)b->offset + (off_t)totalGot);
                if (got < 0) {
                    if (errno == EINTR) continue;
                    ioErr = true;
                    break;
                }
                if (got == 0) {
                    memset(dst + totalGot, 0, want - totalGot);  // past EOF the drive returns adjacent data
                    break;
                }
                totalGot += (size_t)got;
            }
            if (ioErr) {
                result = -1;
            } else {
                b->transferredSize = b->length;
                result = (s32)b->length;
            }
        }
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            b->currTransferSize = (u32)result;
            sActive = nullptr;
            sDone.push_back(b);
        }
        port_irq_raise(PORT_IRQ_DVD);
    }
}

static void dvdIrq() {
    for (;;) {
        DVDCommandBlock* b;
        {
            std::lock_guard<std::mutex> lk(sQueueMutex);
            if (sDone.empty()) {
                break;
            }
            b = sDone.front();
            sDone.pop_front();
        }
        s32 result = (s32)b->currTransferSize;
        b->state = result < 0 ? kStateFatal : kStateEnd;
        if (b->callback) {
            b->callback(result, b);
        }
    }
    OSWakeupThread(&sSyncQueue);
}

static void submit(DVDCommandBlock* b, u32 entry) {
    b->state = kStateBusy;
    b->transferredSize = 0;
    std::lock_guard<std::mutex> lk(sQueueMutex);
    sPending.push_back({b, entry});
    sQueueCv.notify_one();
}

// DVDFileInfo embeds its command block first; the file callback gets the
// file info back (as on hardware).
static void fileCallback(s32 result, DVDCommandBlock* block) {
    DVDFileInfo* fi = (DVDFileInfo*)block;
    if (fi->callback) {
        fi->callback(result, fi);
    }
}

// ---------------------------------------------------------------------------
// SDK API
// ---------------------------------------------------------------------------
extern "C" {

void DVDInit(void) {
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    OSInitThreadQueue(&sSyncQueue);
    if (!loadFst()) {
        port_fatal("dvd: game data not found under %s (expected sys/fst.bin and files/)", sDataRoot.c_str());
    }
    port_irq_set_handler(PORT_IRQ_DVD, dvdIrq);
    PortHostAllocScope scope;
    std::thread(workerMain).detach();
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    PortHostAllocScope scope;
    if (entrynum < 0 || (u32)entrynum >= sFst.size() || sFst[entrynum].isDir) {
        return FALSE;
    }
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = (u32)entrynum;
    fileInfo->length = (u32)entrySize((u32)entrynum);
    fileInfo->callback = nullptr;
    fileInfo->cb.state = kStateEnd;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    PortHostAllocScope scope;
    s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        port_log("dvd: file not found: %s", fileName);
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    (void)fileInfo;
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    (void)prio;
    fileInfo->callback = callback;
    DVDCommandBlock* b = &fileInfo->cb;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = (u32)offset;
    b->callback = fileCallback;
    submit(b, fileInfo->startAddr);
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    DVDCommandBlock* b = &fileInfo->cb;
    fileInfo->callback = nullptr;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = (u32)offset;
    b->callback = nullptr;
    submit(b, fileInfo->startAddr);
    BOOL en = OSDisableInterrupts();
    while (b->state == kStateBusy || b->state == kStateWaiting) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    return b->state == kStateEnd ? (s32)b->transferredSize : -1;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        for (auto it = sPending.begin(); it != sPending.end(); ++it) {
            if (it->block == block) {
                sPending.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        block->state = kStateCanceled;
    }
    if (callback) {
        callback(0, block);
    }
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* block) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(sQueueMutex);
        for (auto it = sPending.begin(); it != sPending.end(); ++it) {
            if (it->block == block) {
                sPending.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) {
        block->state = kStateCanceled;
        return 0;
    }
    // In flight: wait for it to finish.
    BOOL en = OSDisableInterrupts();
    while (block->state == kStateBusy) {
        OSSleepThread(&sSyncQueue);
    }
    OSRestoreInterrupts(en);
    return 0;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block->state; }

s32 DVDGetDriveStatus(void) {
    std::lock_guard<std::mutex> lk(sQueueMutex);
    return (sPending.empty() && sActive == nullptr) ? 0 : 1;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    block->state = kStateEnd;
    if (callback) {
        callback(1, block);
    }
    return TRUE;
}

BOOL DVDOpenDir(const char* dirName, DVDDir* dir) {
    s32 entry = DVDConvertPathToEntrynum(dirName);
    if (entry < 0 || !sFst[entry].isDir) {
        return FALSE;
    }
    dir->entryNum = (u32)entry;
    dir->location = (u32)entry + 1;
    dir->next = sFst[entry].next;
    return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    u32 loc = dir->location;
    if (loc <= dir->entryNum || loc >= dir->next) {
        return FALSE;
    }
    FstEntry& fe = sFst[loc];
    dirent->entryNum = loc;
    dirent->isDir = fe.isDir;
    dirent->name = (char*)fe.name.c_str();
    dir->location = fe.isDir ? fe.next : loc + 1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir* dir) {
    (void)dir;
    return TRUE;
}

BOOL DVDChangeDir(const char* dirName) {
    s32 entry = DVDConvertPathToEntrynum(dirName);
    if (entry < 0 || !sFst[entry].isDir) {
        return FALSE;
    }
    sCurrentDir = (u32)entry;
    return TRUE;
}

}  // extern "C"
