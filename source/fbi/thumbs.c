#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "thumbs.h"
#include "viewers.h"
#include "../core/core.h"
#include "../libs/stb_image/stb_image.h"

#define THUMB_SIZE 96
#define THUMB_SLOTS 48
#define THUMB_MAX_FILE (8 * 1024 * 1024)
#define THUMB_MAX_PIXELS 3000000
#define THUMB_STACK 0x20000
#define THUMB_PRIORITY 0x32

typedef enum {
    THUMB_FREE,
    THUMB_QUEUED,
    THUMB_DECODING,
    THUMB_DECODED,   // Pixels waiting for the UI thread to make the texture.
    THUMB_READY,
    THUMB_FAILED
} thumb_state;

typedef struct {
    thumb_state state;
    char path[FILE_PATH_MAX];
    u64 size;
    FS_Archive archive;
    u8* pixels;
    u32 texture;
    u64 used;
} thumb;

static thumb thumbs[THUMB_SLOTS];
static LightLock thumbs_lock;
static bool thumbs_started = false;
static Thread thumbs_thread = NULL;
static volatile bool thumbs_quit = false;

// RGBA of the picture's centre, THUMB_SIZE square, in the byte order the GPU wants; NULL if it cannot.
static u8* thumbs_decode(FS_Archive archive, const char* path) {
    u8* file = NULL;
    u32 size = 0;
    u64 full = 0;
    if(R_FAILED(viewers_read_file(archive, path, THUMB_MAX_FILE, &file, &size, &full)) || full > size) {
        free(file);
        return NULL;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    if(!stbi_info_from_memory(file, (int) size, &w, &h, &comp) || w <= 0 || h <= 0 || (u64) w * (u64) h > THUMB_MAX_PIXELS) {
        free(file);
        return NULL;
    }

    u8* src = stbi_load_from_memory(file, (int) size, &w, &h, &comp, 4);
    free(file);
    if(src == NULL) {
        return NULL;
    }

    u8* out = (u8*) malloc(THUMB_SIZE * THUMB_SIZE * 4);
    if(out == NULL) {
        free(src);
        return NULL;
    }

    int side = w < h ? w : h;
    int left = (w - side) / 2;
    int top = (h - side) / 2;
    for(int dy = 0; dy < THUMB_SIZE; dy++) {
        int y0 = top + (int) ((long long) dy * side / THUMB_SIZE);
        int y1 = top + (int) ((long long) (dy + 1) * side / THUMB_SIZE);
        y1 = y1 <= y0 ? y0 + 1 : y1;
        for(int dx = 0; dx < THUMB_SIZE; dx++) {
            int x0 = left + (int) ((long long) dx * side / THUMB_SIZE);
            int x1 = left + (int) ((long long) (dx + 1) * side / THUMB_SIZE);
            x1 = x1 <= x0 ? x0 + 1 : x1;

            u32 sum[4] = {0, 0, 0, 0};
            for(int y = y0; y < y1; y++) {
                const u8* row = src + ((size_t) y * w + x0) * 4;
                for(int x = x0; x < x1; x++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }

            u32 n = (u32) ((y1 - y0) * (x1 - x0));
            u8* dst = out + ((size_t) dy * THUMB_SIZE + dx) * 4;
            // A, B, G, R.
            dst[0] = (u8) (sum[3] / n);
            dst[1] = (u8) (sum[2] / n);
            dst[2] = (u8) (sum[1] / n);
            dst[3] = (u8) (sum[0] / n);
        }
    }

    free(src);
    return out;
}

static void thumbs_worker(void* arg) {
    while(!thumbs_quit) {
        // The most recently asked for first: what is on screen now.
        LightLock_Lock(&thumbs_lock);
        thumb* next = NULL;
        for(u32 i = 0; i < THUMB_SLOTS; i++) {
            if(thumbs[i].state == THUMB_QUEUED && (next == NULL || thumbs[i].used > next->used)) {
                next = &thumbs[i];
            }
        }

        char path[FILE_PATH_MAX];
        FS_Archive archive = 0;
        if(next != NULL) {
            next->state = THUMB_DECODING;
            string_copy(path, next->path, sizeof(path));
            archive = next->archive;
        }

        LightLock_Unlock(&thumbs_lock);

        if(next == NULL) {
            svcSleepThread(30000000);
            continue;
        }

        u8* pixels = thumbs_decode(archive, path);

        LightLock_Lock(&thumbs_lock);
        // The slot is ours until it is decoded: nobody else changes a decoding one.
        next->pixels = pixels;
        next->state = pixels != NULL ? THUMB_DECODED : THUMB_FAILED;
        LightLock_Unlock(&thumbs_lock);

        fs_close_archive(archive);
    }
}

static void thumbs_start() {
    thumbs_started = true;
    LightLock_Init(&thumbs_lock);

    bool isNew = false;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thumbs_thread = threadCreate(thumbs_worker, NULL, THUMB_STACK, THUMB_PRIORITY, 2, false);
    }

    if(thumbs_thread == NULL) {
        thumbs_thread = threadCreate(thumbs_worker, NULL, THUMB_STACK, THUMB_PRIORITY, -2, false);
    }
}

static void thumbs_release(thumb* t) {
    if(t->texture != 0) {
        screen_unload_texture(t->texture);
    }

    free(t->pixels);
    memset(t, 0, sizeof(*t));
}

u32 thumbs_get(FS_Archive archive, const char* path, u64 size) {
    if(size == 0 || size > THUMB_MAX_FILE) {
        return 0;
    }

    if(!thumbs_started) {
        thumbs_start();
        if(thumbs_thread == NULL) {
            return 0;
        }
    }

    u64 now = osGetTime();
    u32 texture = 0;

    LightLock_Lock(&thumbs_lock);
    thumb* found = NULL;
    thumb* spare = NULL;
    thumb* oldest = NULL;
    for(u32 i = 0; i < THUMB_SLOTS; i++) {
        thumb* t = &thumbs[i];
        if(t->state == THUMB_FREE) {
            spare = spare != NULL ? spare : t;
        } else if(t->size == size && strcmp(t->path, path) == 0) {
            found = t;
            break;
        } else if(t->state != THUMB_QUEUED && t->state != THUMB_DECODING && now - t->used > 1500 && (oldest == NULL || t->used < oldest->used)) {
            oldest = t;
        }
    }

    if(found != NULL) {
        found->used = now;
        if(found->state == THUMB_DECODED) {
            // The texture is made here, on the thread that draws.
            u32 id = screen_allocate_free_texture();
            screen_load_texture_untiled(id, found->pixels, THUMB_SIZE * THUMB_SIZE * 4, THUMB_SIZE, THUMB_SIZE, GPU_RGBA8, true);
            free(found->pixels);
            found->pixels = NULL;
            found->texture = id;
            found->state = THUMB_READY;
        }

        texture = found->state == THUMB_READY ? found->texture : 0;
    } else {
        thumb* slot = spare != NULL ? spare : oldest;
        if(slot != NULL && R_SUCCEEDED(fs_ref_archive(archive))) {
            thumbs_release(slot);
            slot->state = THUMB_QUEUED;
            string_copy(slot->path, path, FILE_PATH_MAX);
            slot->size = size;
            slot->archive = archive;
            slot->used = now;
        }
    }

    LightLock_Unlock(&thumbs_lock);
    return texture;
}

void thumbs_exit() {
    if(!thumbs_started) {
        return;
    }

    thumbs_quit = true;
    if(thumbs_thread != NULL) {
        threadJoin(thumbs_thread, U64_MAX);
        threadFree(thumbs_thread);
        thumbs_thread = NULL;
    }

    for(u32 i = 0; i < THUMB_SLOTS; i++) {
        thumbs_release(&thumbs[i]);
    }

    thumbs_started = false;
    thumbs_quit = false;
}
