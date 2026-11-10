// NSA's forwarder: the program inside the CIAs NSA makes for a .3dsx (SD card -> a .3dsx's actions -> "Make a HOME
// Menu icon"). It only starts that .3dsx, which stays where it is on the SD card, through Luma3DS's 3DSX loader as NSA's
// own "Run" does (source/fbi/launch3dsx.c): the file is set as the loader's target, then the Homebrew Launcher
// placeholder title is started and the loader puts the .3dsx in its place.
//
// NSA writes the path after the marker below in each copy (source/fbi/forwarder.c), so .code must be stored
// uncompressed (forwarder.rsf). Rebuild romfs/forwarder.cia with `make -C forwarder` after changing this file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#define FWD_MAGIC "NSAFWD01"
#define FWD_PATH_MAX 512
#define FWD_DEFAULT_TID 0x000400000D921E00ULL
#define FWD_TID_FILE "sdmc:/nsa/hbl_title.txt"
#define FWD_ARGV_SIZE 0x400

// The marker, then the .3dsx's path on the SD card ("/3ds/foo.3dsx"). Volatile: the compiler must read what NSA wrote,
// not the empty string it sees here.
__attribute__((used, aligned(4))) volatile char fwd_target[sizeof(FWD_MAGIC) - 1 + FWD_PATH_MAX] = FWD_MAGIC;

static u64 fwd_wanted_tid() {
    u64 tid = FWD_DEFAULT_TID;
    FILE* file = fopen(FWD_TID_FILE, "r");
    if(file != NULL) {
        char text[40] = {0};
        if(fgets(text, sizeof(text), file) != NULL) {
            u64 value = strtoull(text, NULL, 16);
            if(value != 0) {
                tid = value;
            }
        }

        fclose(file);
    }

    return tid;
}

static bool fwd_find_title(u64* tid, FS_MediaType* media) {
    u64 wanted = fwd_wanted_tid();
    static const FS_MediaType medias[] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    for(u32 m = 0; m < 2; m++) {
        u32 count = 0;
        if(R_FAILED(AM_GetTitleCount(medias[m], &count)) || count == 0) {
            continue;
        }

        u64* ids = (u64*) calloc(count, sizeof(u64));
        if(ids == NULL) {
            continue;
        }

        u32 read = 0;
        if(R_SUCCEEDED(AM_GetTitleList(&read, medias[m], count, ids))) {
            for(u32 i = 0; i < read; i++) {
                // The loader ignores the platform bits of the title ID.
                if(((ids[i] ^ wanted) & ~0xF0000000ULL) == 0) {
                    *tid = ids[i];
                    *media = medias[m];
                    free(ids);
                    return true;
                }
            }
        }

        free(ids);
    }

    return false;
}

static Result fwd_set_target(Handle handle, const char* path) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmdbuf[2] = (u32) path;

    Result res = svcSendSyncRequest(handle);
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

static Result fwd_set_argv(Handle handle, const void* buffer, size_t size) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(size, 1);
    cmdbuf[2] = (u32) buffer;

    Result res = svcSendSyncRequest(handle);
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

// Something is missing: say what on the top screen until a button is pressed.
static void fwd_fail(const char* path, const char* what, Result res) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[2;2HNSA forwarder\n\n");
    printf("  %s\n\n", what);
    if(path[0] != '\0') {
        printf("  %s\n\n", path);
    }

    if(res != 0) {
        printf("  Error %08lX\n\n", res);
    }

    printf("  Press any button to go back.\n");
    while(aptMainLoop()) {
        hidScanInput();
        if(hidKeysDown() != 0) {
            break;
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
}

int main(int argc, char** argv) {
    char path[FWD_PATH_MAX + 1] = {0};
    for(u32 i = 0; i < FWD_PATH_MAX; i++) {
        path[i] = fwd_target[sizeof(FWD_MAGIC) - 1 + i];
        if(path[i] == '\0') {
            break;
        }
    }

    if(path[0] == '\0') {
        fwd_fail(path, "This forwarder has no program set.", 0);
        return 0;
    }

    char full[FWD_PATH_MAX + 8];
    snprintf(full, sizeof(full), "sdmc:%s", path);
    struct stat st;
    if(stat(full, &st) != 0) {
        fwd_fail(path, "The program is not on the SD card any more:", 0);
        return 0;
    }

    Result res = amInit();
    if(R_FAILED(res)) {
        fwd_fail(path, "Could not list the installed titles.", res);
        return 0;
    }

    u64 tid = 0;
    FS_MediaType media = MEDIATYPE_SD;
    bool found = fwd_find_title(&tid, &media);
    amExit();
    if(!found) {
        fwd_fail(path, "The Homebrew Launcher title is not installed. Install its CIA, or put its title ID in sdmc:/nsa/hbl_title.txt.", 0);
        return 0;
    }

    Handle handle = 0;
    res = svcConnectToPort(&handle, "hb:ldr");
    if(R_FAILED(res)) {
        fwd_fail(path, "This console has no 3DSX loader (Luma3DS 9 or newer).", res);
        return 0;
    }

    // argv for the program: the count, then its strings; the first is its own path.
    static u8 args[FWD_ARGV_SIZE];
    u32 count = 1;
    memcpy(args, &count, sizeof(count));
    snprintf((char*) args + sizeof(count), sizeof(args) - sizeof(count), "%s", full);

    if(R_SUCCEEDED(res = fwd_set_target(handle, path))) {
        res = fwd_set_argv(handle, args, sizeof(args));
    }

    svcCloseHandle(handle);
    if(R_FAILED(res)) {
        fwd_fail(path, "The 3DSX loader refused the program.", res);
        return 0;
    }

    aptSetChainloader(tid, media);
    return 0;
}
