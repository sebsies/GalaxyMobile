// NAND flash emulation: save data lives in files under the app's storage.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/nand.h"

static std::string sNandRoot;
static const char kHomeDir[] = "/title/00010000/524d4750/data";

extern "C" void port_nand_set_root(const char* root) { sNandRoot = root; }

static std::string hostPath(const char* path) {
    PortHostAllocScope scope;
    std::string p = path ? path : "";
    if (p.empty() || p[0] != '/') {
        p = std::string(kHomeDir) + "/" + p;
    }
    return sNandRoot + p;
}

static void makeDirs(const std::string& path) {
    PortHostAllocScope scope;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            std::string d = path.substr(0, i);
            mkdir(d.c_str(), 0770);
        }
    }
}

static s32 errnoToNand(int e) {
    switch (e) {
    case ENOENT:
        return NAND_RESULT_NOEXISTS;
    case EEXIST:
        return NAND_RESULT_EXISTS;
    case EACCES:
    case EPERM:
        return NAND_RESULT_ACCESS;
    case ENOSPC:
        return NAND_RESULT_MAXBLOCKS;
    case ENOTEMPTY:
        return NAND_RESULT_NOTEMPTY;
    default:
        return NAND_RESULT_UNKNOWN;
    }
}

extern "C" {

s32 NANDInit(void) { return NAND_RESULT_OK; }

s32 NANDGetHomeDir(char path[NAND_MAX_PATH]) {
    strcpy(path, kHomeDir);
    return NAND_RESULT_OK;
}

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    (void)perm;
    (void)attr;
    std::string hp = hostPath(path);
    makeDirs(hp);
    int fd = open(hp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
    if (fd < 0) {
        return errnoToNand(errno);
    }
    close(fd);
    return NAND_RESULT_OK;
}

s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr) { return NANDCreate(path, perm, attr); }

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode) {
    std::string hp = hostPath(path);

    if ((mode == NAND_ACCESS_READ || mode == NAND_ACCESS_RW) && strstr(path, "GameData.bin")) {
        struct stat st;
        if (stat(hp.c_str(), &st) != 0 || st.st_size < 0x140) {
            std::string bak = hp + ".bak";
            struct stat stBak;
            if (stat(bak.c_str(), &stBak) == 0 && stBak.st_size >= 0x140) {
                // Restore from backup
                FILE* src = fopen(bak.c_str(), "rb");
                if (src) {
                    makeDirs(hp);
                    FILE* dst = fopen(hp.c_str(), "wb");
                    if (dst) {
                        char buf[4096];
                        size_t r;
                        while ((r = fread(buf, 1, sizeof(buf), src)) > 0) {
                            fwrite(buf, 1, r, dst);
                        }
                        fclose(dst);
                    }
                    fclose(src);
                }
            }
        }
    }

    int flags = O_CLOEXEC;
    switch (mode) {
    case NAND_ACCESS_READ:
        flags |= O_RDONLY;
        break;
    case NAND_ACCESS_WRITE:
        flags |= O_WRONLY;
        break;
    default:
        flags |= O_RDWR;
        break;
    }
    int fd = open(hp.c_str(), flags);
    if (fd < 0) {
        return errnoToNand(errno);
    }
    memset(info, 0, sizeof(*info));
    info->fileDescriptor = fd;
    info->accType = mode;
    strncpy(info->origPath, path, NAND_MAX_PATH - 1);
    return NAND_RESULT_OK;
}

s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 mode) { return NANDOpen(path, info, mode); }

s32 NANDClose(NANDFileInfo* info) {
    if (info->fileDescriptor >= 0) {
        if (info->accType != NAND_ACCESS_READ) {
            off_t pos = lseek(info->fileDescriptor, 0, SEEK_CUR);
            if (pos > 0) {
                ftruncate(info->fileDescriptor, pos);
            }
            fsync(info->fileDescriptor);

            if (strstr(info->origPath, "GameData.bin")) {
                std::string hp = hostPath(info->origPath);
                std::string bak = hp + ".bak";
                FILE* src = fopen(hp.c_str(), "rb");
                if (src) {
                    FILE* dst = fopen(bak.c_str(), "wb");
                    if (dst) {
                        char buf[4096];
                        size_t r;
                        while ((r = fread(buf, 1, sizeof(buf), src)) > 0) {
                            fwrite(buf, 1, r, dst);
                        }
                        fclose(dst);
                    }
                    fclose(src);
                }
            }
        }
        close(info->fileDescriptor);
        info->fileDescriptor = -1;
    }
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    u8* ptr = (u8*)buf;
    u32 total = 0;
    while (total < length) {
        ssize_t n = read(info->fileDescriptor, ptr + total, length - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errnoToNand(errno);
        }
        if (n == 0) {
            break;
        }
        total += (u32)n;
    }
    return (s32)total;
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    const u8* ptr = (const u8*)buf;
    u32 total = 0;
    while (total < length) {
        ssize_t n = write(info->fileDescriptor, ptr + total, length - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errnoToNand(errno);
        }
        if (n == 0) {
            break;
        }
        total += (u32)n;
    }
    return (s32)total;
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    off_t r = lseek(info->fileDescriptor, offset, whence);
    return r < 0 ? errnoToNand(errno) : (s32)r;
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    struct stat st;
    if (fstat(info->fileDescriptor, &st) != 0) {
        return errnoToNand(errno);
    }
    *length = (u32)st.st_size;
    return NAND_RESULT_OK;
}

static int removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        return rmdir(path.c_str());
    }
    return unlink(path.c_str());
}

s32 NANDDelete(const char* path) {
    PortHostAllocScope scope;
    std::string hp = hostPath(path);
    if (removeTree(hp) != 0) {
        return errnoToNand(errno);
    }
    return NAND_RESULT_OK;
}

s32 NANDMove(const char* path, const char* destDir) {
    PortHostAllocScope scope;
    std::string src = hostPath(path);
    std::string name = src.substr(src.rfind('/') + 1);
    std::string dst = hostPath(destDir) + "/" + name;
    makeDirs(dst);
    if (rename(src.c_str(), dst.c_str()) != 0) {
        return errnoToNand(errno);
    }
    return NAND_RESULT_OK;
}

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) {
    (void)fsBlock;
    (void)inode;
    *answer = 0;  // enough space and inodes
    return NAND_RESULT_OK;
}

void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment) {
    memset(banner, 0, sizeof(*banner));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    for (int i = 0; title && i < NAND_BANNER_COMMENT_SIZE && title[i]; i++) {
        banner->comment[0][i] = title[i];
    }
    for (int i = 0; comment && i < NAND_BANNER_COMMENT_SIZE && comment[i]; i++) {
        banner->comment[1][i] = comment[i];
    }
}

}  // extern "C"