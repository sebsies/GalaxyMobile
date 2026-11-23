// Brings up the emulated Wii environment and starts the game's main().
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "port/port.h"

extern "C" void port_os_time_init(void);
extern "C" void port_gx_init(void);
extern "C" void port_gx_use_recorder_backend(void);
extern "C" void port_dvd_set_root(const char* root);
extern "C" void port_dvd_load_dol_data(const char* root);
extern "C" void port_nand_set_root(const char* root);

// GameSystem.cpp's main(), renamed for the port.
void port_game_main(void);

static void gameEntry() { port_game_main(); }

static void ensureDirRecursive(const std::string& path) {
    size_t pos = 0;
    while (pos < path.size()) {
        pos = path.find('/', pos + 1);
        std::string sub = (pos == std::string::npos) ? path : path.substr(0, pos);
        if (!sub.empty()) {
            mkdir(sub.c_str(), 0755);
        }
    }
}

static void copyFileIfMissing(const std::string& src, const std::string& dst) {
    struct stat st;
    if (stat(dst.c_str(), &st) == 0) return;
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return;
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[16384];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(out);
    fclose(in);
}

extern "C" void port_boot(const char* dataRoot, const char* saveRoot) {
    port_log("port_boot: data=%s save=%s", dataRoot, saveRoot);
    port_mem_init();
    port_os_time_init();
    port_dvd_set_root(dataRoot);
    const PortDisc* disc = port_dvd_disc();
    if (!disc) {
        port_fatal("%s: not the files of a Super Mario Galaxy disc the port knows (no EuEnglish, UsEnglish, JpJapanese or KrKorean "
                   "folder with the game's texts in sys/fst.bin and files/)",
                   dataRoot);
    }
    port_log("disc: %s (%s), %d languages", disc->id, disc->region, port_language_count());
    port_mem_set_disc_id(disc->id);
    port_dvd_load_dol_data(dataRoot);

    std::string effectiveSaveRoot = saveRoot ? saveRoot : "";
    char modSaveId[64] = {};
    int cloneVanilla = 1;
    if (port_mods_get_active_save(modSaveId, sizeof(modSaveId), &cloneVanilla) && modSaveId[0]) {
        effectiveSaveRoot = std::string(saveRoot) + "_mods/" + modSaveId;
        ensureDirRecursive(effectiveSaveRoot);
        if (cloneVanilla) {
            copyFileIfMissing(std::string(saveRoot) + "/GameData.bin", effectiveSaveRoot + "/GameData.bin");
        }
        port_log("mods: using isolated Riivolution save root %s (clone=%d)", effectiveSaveRoot.c_str(), cloneVanilla);
    }

    port_nand_set_root(effectiveSaveRoot.c_str());
    port_gx_init();
    port_gx_use_recorder_backend();
    port_os_boot(gameEntry);
}
