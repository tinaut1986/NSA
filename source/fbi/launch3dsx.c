#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "launch3dsx.h"
#include "relaunch.h"
#include "../core/core.h"

// Luma3DS's default placeholder title (hbldr_3dsx_tid); `sdmc:/nsa/hbl_title.txt` can hold another
// (16 hex digits) for a console where Luma was set to a different one.
#define LAUNCH_DEFAULT_TID 0x000400000D921E00ULL
#define LAUNCH_ARGV_SIZE 0x400
#define LAUNCH_TID_FILE "sdmc:/nsa/hbl_title.txt"

static volatile bool launch_exit = false;

static u64 launch_wanted_tid() {
    u64 tid = LAUNCH_DEFAULT_TID;
    FILE* file = fopen(LAUNCH_TID_FILE, "r");
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

// The loader ignores the platform bits of the title ID.
static bool launch_same_title(u64 a, u64 b) {
    return ((a ^ b) & ~0xF0000000ULL) == 0;
}

// Where the placeholder title is installed; false when it is not.
static bool launch_find_title(u64* tid, FS_MediaType* media) {
    u64 wanted = launch_wanted_tid();
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
                if(launch_same_title(ids[i], wanted)) {
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

static Result launch_set_target(Handle handle, const char* path) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmdbuf[2] = (u32) path;

    Result res = svcSendSyncRequest(handle);
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

static Result launch_set_argv(Handle handle, const void* buffer, size_t size) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(size, 1);
    cmdbuf[2] = (u32) buffer;

    Result res = svcSendSyncRequest(handle);
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

bool launch3dsx_available(char* why, size_t size) {
    Handle handle = 0;
    Result res = svcConnectToPort(&handle, "hb:ldr");
    if(R_FAILED(res)) {
        snprintf(why, size, _("This console has no 3DSX loader (Luma3DS 9 or newer with it on) or NSA cannot reach it: %08lX"), res);
        return false;
    }

    svcCloseHandle(handle);

    if(envIsHomebrew()) {
        return true;
    }

    u64 tid = 0;
    FS_MediaType media = MEDIATYPE_SD;
    if(!launch_find_title(&tid, &media)) {
        snprintf(why, size, "%s", _("The Homebrew Launcher title is not installed. Install its CIA, or put its title ID in sdmc:/nsa/hbl_title.txt."));
        return false;
    }

    return true;
}

Result launch3dsx_start(const char* path) {
    u64 tid = 0;
    FS_MediaType media = MEDIATYPE_SD;
    bool self = envIsHomebrew();
    if(!self && !launch_find_title(&tid, &media)) {
        return R_APP_NOT_IMPLEMENTED;
    }

    Handle handle = 0;
    Result res = svcConnectToPort(&handle, "hb:ldr");
    if(R_FAILED(res)) {
        return res;
    }

    // argv for the program: the count, then its strings; the first is its own path.
    u8* argv = (u8*) calloc(1, LAUNCH_ARGV_SIZE);
    if(argv == NULL) {
        svcCloseHandle(handle);
        return R_APP_OUT_OF_MEMORY;
    }

    u32 count = 1;
    memcpy(argv, &count, sizeof(count));
    snprintf((char*) argv + sizeof(count), LAUNCH_ARGV_SIZE - sizeof(count), "sdmc:%s", path);

    if(R_SUCCEEDED(res = launch_set_target(handle, path)) && R_SUCCEEDED(res = launch_set_argv(handle, argv, LAUNCH_ARGV_SIZE))) {
        if(self) {
            aptSetChainloaderToSelf();
        } else {
            aptSetChainloader(tid, media);
        }

        launch_exit = true;
    }

    free(argv);
    svcCloseHandle(handle);
    return res;
}

bool launch3dsx_poll() {
    return launch_exit;
}
