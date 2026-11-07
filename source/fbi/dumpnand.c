#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "resources.h"
#include "section.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../core/speedlog.h"

// Dump NAND: its own screen. It shows the space the image needs against what the SD card has,
// where the file will go, the progress with speed and time left, and at the end how long it
// took and the SHA-256 of what was read. The image is the raw NAND as the system serves it:
// the whole of it, installed things and free space alike.

typedef enum {
    DUMP_READY,
    DUMP_NO_SPACE,
    DUMP_WORKING,
    DUMP_DONE
} dumpnand_state;

#define DUMP_SLOTS 4
#define DUMP_BLOCK (1024 * 1024)

typedef struct {
    data_op_data op;   // Progress, cancel and result, as the screen reads them.

    dumpnand_state state;
    u64 nandSize;
    u64 sdFree;
    bool sizeKnown;

    char path[FILE_PATH_MAX];
    u64 startTime;
    u64 elapsedMs;

    // The copy (see "the copy" below).
    u8* block[DUMP_SLOTS];
    u32 blockSize[DUMP_SLOTS];      // 0: the end, nothing more comes.
    u32 blockUsers[DUMP_SLOTS];     // Of the writer and the hasher, still to take it.
    LightSemaphore freeBlocks;
    LightSemaphore toWrite;
    LightSemaphore toHash;
    u32 srcHandle;
    u32 dstHandle;
    volatile Result writeRes;
    Thread writer;
    Thread hasher;
    LightLock traceLock;
    speedlog* trace;
    u64 hashTicks;

    // Hashed in order, on its own thread.
    mbedtls_sha256_context sha;
    char hash[65];

    widget_press press;
} dumpnand_data;

#define CARD_HEIGHT 76
#define CELL_HEIGHT 34
#define BUTTON_HEIGHT 30

// ---- the copy ----
//
// Three threads, so that reading the NAND, writing the SD card and hashing overlap: a reader fills
// DUMP_SLOTS blocks in turn, and a writer and a hasher take each block in order and hand it back
// once both are done with it. Measured on the New 3DS XL (1.84 GiB) with the copy of data_op (one
// thread, 256 KB blocks): 198 s reading and 449 s writing, 2.99 MiB/s without the hash, and about
// half that with the hash on the system core (the app gets 30% of it, at 268 MHz). The hasher runs
// on core 2 of a New 3DS (on the app core otherwise, below the UI). The New 3DS's faster clock and
// L2 cache are main.c's, on for all of NSA's life: never turned off here (it slowed everything after
// a dump, FTP downloads from 1.3 MB/s to 0.46).

static Result dumpnand_open_src(void* data, u32 index, u32* handle) {
    return FSUSER_OpenFileDirectly(handle, ARCHIVE_NAND_W_FS, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_UTF16, u"/"), FS_OPEN_READ, 0);
}

static Result dumpnand_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result dumpnand_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result dumpnand_open_dst(dumpnand_data* dump, u32* handle) {
    Result res = 0;

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR "nand/"))) {
            FS_Path* fsPath = fs_make_path_utf8(dump->path);
            if(fsPath != NULL) {
                res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

                fs_free_path_utf8(fsPath);
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        FSUSER_CloseArchive(sdmcArchive);
    }

    return res;
}

// The writer and the hasher each give a block back; the second one frees it for the reader.
static void dumpnand_release_block(dumpnand_data* dump, u32 slot) {
    if(__atomic_sub_fetch(&dump->blockUsers[slot], 1, __ATOMIC_ACQ_REL) == 0) {
        LightSemaphore_Release(&dump->freeBlocks, 1);
    }
}

static void dumpnand_trace_disk(dumpnand_data* dump, u64 bytes, u64 ticks) {
    LightLock_Lock(&dump->traceLock);
    speedlog_disk(dump->trace, bytes, ticks);
    LightLock_Unlock(&dump->traceLock);
}

static void dumpnand_trace_net(dumpnand_data* dump, u64 bytes, u64 ticks) {
    LightLock_Lock(&dump->traceLock);
    speedlog_net(dump->trace, bytes, ticks);
    LightLock_Unlock(&dump->traceLock);
}

// After a failed write it goes on taking blocks (without writing them) until the end, so the
// reader never waits for a block that does not come back.
static void dumpnand_writer(void* arg) {
    dumpnand_data* dump = (dumpnand_data*) arg;

    u64 offset = 0;
    u64 lastUpdate = osGetTime();
    u64 bytesSinceUpdate = 0;
    for(u32 slot = 0; ; slot = (slot + 1) % DUMP_SLOTS) {
        LightSemaphore_Acquire(&dump->toWrite, 1);

        u32 size = dump->blockSize[slot];
        if(size == 0) {
            break;
        }

        if(R_SUCCEEDED(dump->writeRes)) {
            u32 written = 0;
            u64 ticks = speedlog_ticks();
            Result res = FSFILE_Write(dump->dstHandle, &written, offset, dump->block[slot], size, 0);
            if(R_SUCCEEDED(res) && written != size) {
                res = R_APP_IO_ERROR;
            }

            dumpnand_trace_disk(dump, written, speedlog_ticks() - ticks);

            if(R_FAILED(res)) {
                dump->writeRes = res;
            } else {
                offset += size;
                dump->op.currProcessed = offset;
                bytesSinceUpdate += size;

                u64 time = osGetTime();
                if(time - lastUpdate >= 1000) {
                    dump->op.bytesPerSecond = (u32) (bytesSinceUpdate * 1000 / (time - lastUpdate));

                    u64 elapsed = time - dump->startTime;
                    dump->op.estimatedRemainingSeconds = elapsed > 0 ? (u32) ((dump->op.currTotal - offset) * elapsed / offset / 1000) : 0;

                    lastUpdate = time;
                    bytesSinceUpdate = 0;
                }
            }
        }

        dumpnand_release_block(dump, slot);
    }
}

static void dumpnand_hasher(void* arg) {
    dumpnand_data* dump = (dumpnand_data*) arg;

    for(u32 slot = 0; ; slot = (slot + 1) % DUMP_SLOTS) {
        LightSemaphore_Acquire(&dump->toHash, 1);

        u32 size = dump->blockSize[slot];
        if(size == 0) {
            break;
        }

        u64 ticks = speedlog_ticks();
        mbedtls_sha256_update_ret(&dump->sha, (const unsigned char*) dump->block[slot], size);
        dump->hashTicks += speedlog_ticks() - ticks;

        dumpnand_release_block(dump, slot);
    }
}

// The end for the writer and the hasher: a block of size 0, then both are waited for.
static void dumpnand_stop_workers(dumpnand_data* dump, u32 slot) {
    if(dump->writer != NULL || dump->hasher != NULL) {
        LightSemaphore_Acquire(&dump->freeBlocks, 1);
        dump->blockSize[slot] = 0;
        LightSemaphore_Release(&dump->toWrite, 1);
        LightSemaphore_Release(&dump->toHash, 1);
    }

    if(dump->writer != NULL) {
        threadJoin(dump->writer, U64_MAX);
        threadFree(dump->writer);
        dump->writer = NULL;
    }

    if(dump->hasher != NULL) {
        threadJoin(dump->hasher, U64_MAX);
        threadFree(dump->hasher);
        dump->hasher = NULL;
    }
}

static void dumpnand_free_resources(dumpnand_data* dump) {
    if(dump->dstHandle != 0) {
        FSFILE_Close(dump->dstHandle);
        dump->dstHandle = 0;
    }

    if(dump->srcHandle != 0) {
        FSFILE_Close(dump->srcHandle);
        dump->srcHandle = 0;
    }

    for(u32 i = 0; i < DUMP_SLOTS; i++) {
        free(dump->block[i]);
        dump->block[i] = NULL;
    }

    if(dump->op.cancelEvent != 0) {
        svcCloseHandle(dump->op.cancelEvent);
        dump->op.cancelEvent = 0;
    }

    aptSetSleepAllowed(true);
}

static void dumpnand_reader(void* arg) {
    dumpnand_data* dump = (dumpnand_data*) arg;

    Result res = 0;
    u64 offset = 0;
    u32 slot = 0;
    while(offset < dump->op.currTotal) {
        if(task_is_quit_all() || svcWaitSynchronization(dump->op.cancelEvent, 0) == 0) {
            res = R_APP_CANCELLED;
            break;
        }

        if(R_FAILED(res = dump->writeRes)) {
            break;
        }

        LightSemaphore_Acquire(&dump->freeBlocks, 1);

        u64 left = dump->op.currTotal - offset;
        u32 size = left < DUMP_BLOCK ? (u32) left : DUMP_BLOCK;
        u32 bytesRead = 0;
        u64 ticks = speedlog_ticks();
        res = FSFILE_Read(dump->srcHandle, &bytesRead, offset, dump->block[slot], size);
        if(R_SUCCEEDED(res) && bytesRead == 0) {
            res = R_APP_IO_ERROR;
        }

        dumpnand_trace_net(dump, bytesRead, speedlog_ticks() - ticks);

        if(R_FAILED(res)) {
            LightSemaphore_Release(&dump->freeBlocks, 1);
            break;
        }

        dump->blockSize[slot] = bytesRead;
        dump->blockUsers[slot] = 2;
        LightSemaphore_Release(&dump->toWrite, 1);
        LightSemaphore_Release(&dump->toHash, 1);

        offset += bytesRead;
        slot = (slot + 1) % DUMP_SLOTS;
    }

    dumpnand_stop_workers(dump, slot);

    if(R_SUCCEEDED(res)) {
        res = dump->writeRes;
    }

    // In the trace the hashing shows as the time spent "waiting" (it overlaps the rest).
    if(dump->trace != NULL) {
        speedlog_wait(dump->trace, dump->hashTicks);
        speedlog_end(dump->trace, R_SUCCEEDED(res));
        dump->trace = NULL;
    }

    dumpnand_free_resources(dump);

    if(res == R_APP_CANCELLED) {
        prompt_display_notify("Failure", "Operation cancelled.", COLOR_TEXT, NULL, NULL, NULL);
    } else if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to dump NAND.");
    }

    dump->op.result = res;
    dump->op.finished = true;
}

// A half image is of no use: it goes (it is NSA's own file).
static void dumpnand_delete_partial(dumpnand_data* dump) {
    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path* fsPath = fs_make_path_utf8(dump->path);
        if(fsPath != NULL) {
            FSUSER_DeleteFile(sdmcArchive, *fsPath);
            fs_free_path_utf8(fsPath);
        }

        FSUSER_CloseArchive(sdmcArchive);
    }
}

// "<hash>  <name>.bin" in <name>.bin.sha256, the format `sha256sum -c` reads.
static void dumpnand_write_hash_file(dumpnand_data* dump) {
    const char* name = strrchr(dump->path, '/');
    name = name != NULL ? name + 1 : dump->path;

    char line[160];
    int length = snprintf(line, sizeof(line), "%s  %s\n", dump->hash, name);

    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "%s.sha256", dump->path);

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            u32 written = 0;
            FSFILE_Write(file, &written, 0, line, (u32) length, FS_WRITE_FLUSH);
            FSFILE_Close(file);
        }

        fs_free_path_utf8(fsPath);
    }
}

// ---- what is known before starting ----

static void dumpnand_probe(dumpnand_data* dump) {
    FS_ArchiveResource resource = {0};
    if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        dump->sdFree = (u64) resource.freeClusters * resource.clusterSize;
    }

    u32 handle = 0;
    if(R_SUCCEEDED(dumpnand_open_src(dump, 0, &handle))) {
        dump->sizeKnown = R_SUCCEEDED(dumpnand_get_src_size(dump, handle, &dump->nandSize));
        dumpnand_close_src(dump, 0, true, handle);
    }

    // A little more than the image: the file's own bookkeeping.
    dump->state = dump->sizeKnown && dump->nandSize + 1024 * 1024 > dump->sdFree ? DUMP_NO_SPACE : DUMP_READY;
}

static void dumpnand_size_text(char* out, size_t size, u64 bytes) {
    snprintf(out, size, "%.2f %s", ui_get_display_size(bytes), ui_get_display_size_units(bytes));
}

static void dumpnand_start(dumpnand_data* dump) {
    time_t t = time(NULL);
    struct tm* timeInfo = localtime(&t);
    strftime(dump->path, sizeof(dump->path), APP_DATA_DIR "nand/NAND_%m-%d-%y_%H-%M-%S.bin", timeInfo);

    mbedtls_sha256_init(&dump->sha);
    mbedtls_sha256_starts_ret(&dump->sha, 0);
    dump->hashTicks = 0;
    dump->hash[0] = '\0';

    data_op_data* op = &dump->op;
    memset(op, 0, sizeof(*op));
    op->finished = true;

    dump->srcHandle = 0;
    dump->dstHandle = 0;
    dump->writer = NULL;
    dump->hasher = NULL;
    dump->writeRes = 0;
    dump->trace = NULL;
    LightLock_Init(&dump->traceLock);
    LightSemaphore_Init(&dump->freeBlocks, DUMP_SLOTS, DUMP_SLOTS);
    LightSemaphore_Init(&dump->toWrite, 0, DUMP_SLOTS);
    LightSemaphore_Init(&dump->toHash, 0, DUMP_SLOTS);

    aptSetSleepAllowed(false);

    Result res = 0;
    for(u32 i = 0; i < DUMP_SLOTS; i++) {
        dump->block[i] = (u8*) memalign(0x1000, DUMP_BLOCK);
        if(dump->block[i] == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    if(R_SUCCEEDED(res)
       && R_SUCCEEDED(res = dumpnand_open_src(dump, 0, &dump->srcHandle))
       && R_SUCCEEDED(res = dumpnand_get_src_size(dump, dump->srcHandle, &op->currTotal))
       && R_SUCCEEDED(res = dumpnand_open_dst(dump, &dump->dstHandle))
       && R_SUCCEEDED(res = svcCreateEvent(&op->cancelEvent, RESET_STICKY))) {
        dump->trace = speedlog_begin("copy", "NAND dump", op->currTotal);
        dump->startTime = osGetTime();
        op->finished = false;

        dump->writer = threadCreate(dumpnand_writer, dump, 0x4000, 0x18, 0, false);
        dump->hasher = threadCreate(dumpnand_hasher, dump, 0x4000, 0x31, 2, false);
        if(dump->hasher == NULL) {
            dump->hasher = threadCreate(dumpnand_hasher, dump, 0x4000, 0x31, 0, false);
        }

        if(dump->writer == NULL || dump->hasher == NULL || threadCreate(dumpnand_reader, dump, 0x10000, 0x18, 1, true) == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
            dumpnand_stop_workers(dump, 0);
        }
    }

    if(R_SUCCEEDED(res)) {
        dump->state = DUMP_WORKING;
    } else {
        bool dstMade = dump->dstHandle != 0;

        if(dump->trace != NULL) {
            speedlog_end(dump->trace, false);
            dump->trace = NULL;
        }

        dumpnand_free_resources(dump);
        if(dstMade) {
            dumpnand_delete_partial(dump);
        }

        mbedtls_sha256_free(&dump->sha);
        op->finished = true;
        error_display_res(NULL, NULL, res, "Failed to initiate NAND dump.");
    }
}

// ---- drawing ----

static void dumpnand_draw_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, w, CELL_HEIGHT, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    widget_text_fit(value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), WIDGET_TEXT_SCALE, t->palette->text, w - 14);
}

static void dumpnand_time_text(char* out, size_t size, u64 ms) {
    u32 seconds = (u32) (ms / 1000);
    if(seconds >= 60) {
        snprintf(out, size, _("%lu min %lu s"), seconds / 60, seconds % 60);
    } else {
        snprintf(out, size, _("%lu s"), seconds);
    }
}

static void dumpnand_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    dumpnand_data* dump = (dumpnand_data*) data;
    const theme_current* t = theme();

    float x = x1 + 10;
    float w = x2 - x1 - 20;
    float y = y1 + 10;
    widget_card(x, y, w, CARD_HEIGHT);

    float iconSize = 56;
    float iconX = x + 10;
    float iconY = y + (CARD_HEIGHT - iconSize) / 2;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    screen_fill_round_rect(iconX, iconY, iconSize, iconSize, t->def->radius, t->accentSoft);
    screen_draw_icon(ICON_DOWNLOAD, iconX + 14, iconY + 14, 28, t->accent);
    screen_set_depth(depth);

    float tx = iconX + iconSize + 12;
    float right = x + w - 10;
    widget_text_fit(_("Dump NAND"), tx, y + 10, WIDGET_TITLE_SCALE, t->palette->text, right - tx);
    screen_draw_text_wrap(_("Copy the whole system memory to the SD card as a backup."), tx, y + 14 + widget_text_height(WIDGET_TITLE_SCALE), WIDGET_TEXT_SCALE, t->palette->textMuted, right);

    char a[48], b[48], c[48];
    const char* labels[3] = {NULL, NULL, NULL};
    float cy = y + CARD_HEIGHT + 8;
    if(dump->state == DUMP_WORKING) {
        dumpnand_size_text(a, sizeof(a), dump->op.currProcessed);
        snprintf(b, sizeof(b), "%.2f %s/s", ui_get_display_size(dump->op.bytesPerSecond), ui_get_display_size_units(dump->op.bytesPerSecond));
        snprintf(c, sizeof(c), "%s", ui_get_display_eta(dump->op.estimatedRemainingSeconds));
        labels[0] = _("Done");
        labels[1] = _("Speed");
        labels[2] = _("Time left");
    } else if(dump->state == DUMP_DONE) {
        dumpnand_size_text(a, sizeof(a), dump->nandSize);
        dumpnand_time_text(b, sizeof(b), dump->elapsedMs);
        snprintf(c, sizeof(c), "%.8s...", dump->hash);
        labels[0] = _("NAND size");
        labels[1] = _("Took");
        labels[2] = "SHA-256";
    } else {
        if(dump->sizeKnown) {
            dumpnand_size_text(a, sizeof(a), dump->nandSize);
        } else {
            snprintf(a, sizeof(a), "?");
        }

        dumpnand_size_text(b, sizeof(b), dump->sdFree);
        c[0] = '\0';
        labels[0] = _("NAND size");
        labels[1] = _("Free (SD)");
    }

    u32 count = labels[2] != NULL ? 3 : 2;
    float cw = (w - (count - 1) * 6) / count;
    dumpnand_draw_cell(x, cy, cw, labels[0], a);
    dumpnand_draw_cell(x + cw + 6, cy, cw, labels[1], b);
    if(count == 3) {
        dumpnand_draw_cell(x + 2 * (cw + 6), cy, cw, labels[2], c);
    }
}

// The two buttons of the bottom screen (or one wide): their rectangles.
static void dumpnand_button_rect(dumpnand_data* dump, u32 i, float bx1, float by2, float bx2, float* x, float* y, float* w) {
    bool two = dump->state == DUMP_READY;
    float gap = 8;
    float full = bx2 - bx1 - 24;
    *w = two ? (full - gap) / 2 : full;
    *x = bx1 + 12 + (two ? (*w + gap) * i : 0);
    *y = by2 - 12 - BUTTON_HEIGHT;
}

static void dumpnand_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    dumpnand_data* dump = (dumpnand_data*) data;
    const theme_current* t = theme();

    float x = x1 + 12;
    float w = x2 - x1 - 24;
    float y = y1 + 8;
    char text[96];
    char value[48];

    if(dump->state == DUMP_READY || dump->state == DUMP_NO_SPACE) {
        bool bad = dump->state == DUMP_NO_SPACE;
        float line = widget_text_height(WIDGET_TEXT_SCALE) + 5;
        float cardH = 8 + 2 * line + 6 + 8 + 6 + widget_text_height(WIDGET_SMALL_SCALE) + 8;
        screen_frame_round_rect(x, y, w, cardH, t->def->radius, t->def->border, bad ? t->palette->danger : t->palette->border, bad ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface);

        float tx = x + 12;
        float right = x + w - 12;

        dumpnand_size_text(value, sizeof(value), dump->nandSize);
        screen_draw_text(_("Needed"), tx, y + 8, WIDGET_TEXT_SCALE, t->palette->text);
        screen_draw_text(value, right - widget_text_width(value, WIDGET_TEXT_SCALE), y + 8, WIDGET_TEXT_SCALE, t->palette->text);

        dumpnand_size_text(value, sizeof(value), dump->sdFree);
        screen_draw_text(_("Free on the SD card"), tx, y + 8 + line, WIDGET_TEXT_SCALE, t->palette->text);
        screen_draw_text(value, right - widget_text_width(value, WIDGET_TEXT_SCALE), y + 8 + line, WIDGET_TEXT_SCALE, bad ? t->palette->danger : t->palette->success);

        float barY = y + 8 + 2 * line + 6;
        float fraction = dump->sdFree != 0 ? (float) ((double) dump->nandSize / (double) dump->sdFree) : 1;
        widget_progress(tx, barY, right - tx, 8, fraction > 1 ? 1 : fraction);

        if(bad) {
            dumpnand_size_text(value, sizeof(value), dump->nandSize + 1024 * 1024 - dump->sdFree);
            snprintf(text, sizeof(text), _("%s missing: free some space first"), value);
        } else {
            dumpnand_size_text(value, sizeof(value), dump->sdFree - dump->nandSize);
            snprintf(text, sizeof(text), _("Fits, with %s left over"), value);
        }

        screen_draw_text(text, tx, barY + 8 + 6, WIDGET_SMALL_SCALE, bad ? t->palette->danger : t->palette->textMuted);

        if(!bad) {
            float pathY = y + cardH + 6;
            widget_card(x, pathY, w, 38);
            screen_draw_text(_("It will be saved in"), tx, pathY + 5, WIDGET_SMALL_SCALE, t->palette->textMuted);
            widget_text_fit("sdmc:" APP_DATA_DIR "nand/", tx, pathY + 5 + widget_text_height(WIDGET_SMALL_SCALE), WIDGET_TEXT_SCALE, t->palette->text, right - tx);
        }
    } else if(dump->state == DUMP_WORKING) {
        float cardH = 128;
        widget_card(x, y, w, cardH);
        float tx = x + 12;
        float right = x + w - 12;
        screen_draw_text(_("Dumping NAND"), tx, y + 8, WIDGET_TEXT_SCALE, t->palette->text);

        float fraction = dump->op.currTotal != 0 ? (float) ((double) dump->op.currProcessed / (double) dump->op.currTotal) : 0;
        widget_progress(tx, y + 8 + widget_text_height(WIDGET_TEXT_SCALE) + 8, right - tx, 12, fraction);

        float lineY = y + 8 + widget_text_height(WIDGET_TEXT_SCALE) + 28;
        snprintf(text, sizeof(text), "%d %%", (int) (fraction * 100));
        screen_draw_text(text, tx, lineY, WIDGET_TEXT_SCALE, t->palette->text);

        char done[32], total[32];
        dumpnand_size_text(done, sizeof(done), dump->op.currProcessed);
        dumpnand_size_text(total, sizeof(total), dump->op.currTotal);
        snprintf(text, sizeof(text), "%s / %s", done, total);
        screen_draw_text(text, right - widget_text_width(text, WIDGET_SMALL_SCALE), lineY + 1, WIDGET_SMALL_SCALE, t->palette->textMuted);

        widget_text_fit(_("Keep the console awake and the SD card in."), tx, y + cardH - 12 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);
    } else {
        float cardH = 128;
        widget_card(x, y, w, cardH);
        float tx = x + 12;
        float right = x + w - 12;
        screen_fill_round_rect(tx, y + 12, 20, 20, 10, t->palette->success);
        screen_draw_icon(ICON_CHECK, tx + 3, y + 15, 14, t->onAccent);
        widget_text_fit(_("NAND dumped"), tx + 30, y + 14, WIDGET_TEXT_SCALE, t->palette->text, right - tx - 30);

        char size[32], took[32];
        dumpnand_size_text(size, sizeof(size), dump->nandSize);
        dumpnand_time_text(took, sizeof(took), dump->elapsedMs);
        snprintf(text, sizeof(text), _("%s in %s"), size, took);
        screen_draw_text(text, tx, y + 42, WIDGET_SMALL_SCALE, t->palette->textMuted);

        screen_draw_text(_("Saved as"), tx, y + 62, WIDGET_SMALL_SCALE, t->palette->textMuted);
        const char* name = strrchr(dump->path, '/');
        widget_text_fit(name != NULL ? name + 1 : dump->path, tx, y + 62 + widget_text_height(WIDGET_SMALL_SCALE), WIDGET_TEXT_SCALE, t->palette->text, right - tx);

        snprintf(text, sizeof(text), "SHA-256 %.16s...", dump->hash);
        widget_text_fit(text, tx, y + cardH - 12 - 2 * widget_text_height(WIDGET_SMALL_SCALE) - 2, WIDGET_SMALL_SCALE, t->accent, right - tx);
        widget_text_fit(_("Also saved as a .sha256 file next to it."), tx, y + cardH - 10 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);
    }

    // Buttons.
    char label[64];
    float bx, by, bw;
    dumpnand_button_rect(dump, 0, x1, y2, x2, &bx, &by, &bw);
    if(dump->state == DUMP_READY) {
        snprintf(label, sizeof(label), "  %s", _("Start dump"));
        widget_button_centered(bx, by, bw, BUTTON_HEIGHT, label, true);
        widget_press_draw(&dump->press, 0, bx, by, bw, BUTTON_HEIGHT, t->def->radiusSmall, t->onAccent, false);
        dumpnand_button_rect(dump, 1, x1, y2, x2, &bx, &by, &bw);
        snprintf(label, sizeof(label), "  %s", _("Cancel"));
        widget_button_centered(bx, by, bw, BUTTON_HEIGHT, label, false);
        widget_press_draw(&dump->press, 1, bx, by, bw, BUTTON_HEIGHT, t->def->radiusSmall, t->palette->text, false);
    } else {
        const char* text2 = dump->state == DUMP_DONE ? _("OK") : dump->state == DUMP_WORKING ? _("Cancel") : _("Back");
        snprintf(label, sizeof(label), "%s  %s", dump->state == DUMP_DONE ? "" : "", text2);
        bool primary = dump->state == DUMP_DONE;
        widget_button_centered(bx, by, bw, BUTTON_HEIGHT, label, primary);
        widget_press_draw(&dump->press, 0, bx, by, bw, BUTTON_HEIGHT, t->def->radiusSmall, primary ? t->onAccent : t->palette->text, false);
    }
}

// ---- input ----

static void dumpnand_close(ui_view* view, dumpnand_data* dump) {
    ui_pop();
    ui_destroy(view);
    free(dump);
}

// What a button press does, by state: 0 the main button, 1 the other one.
static bool dumpnand_act(ui_view* view, dumpnand_data* dump, u32 button) {
    switch(dump->state) {
        case DUMP_READY:
            if(button == 0) {
                dumpnand_start(dump);
                return false;
            }

            dumpnand_close(view, dump);
            return true;
        case DUMP_NO_SPACE:
        case DUMP_DONE:
            dumpnand_close(view, dump);
            return true;
        case DUMP_WORKING:
            svcSignalEvent(dump->op.cancelEvent);
            return false;
    }

    return false;
}

static void dumpnand_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    dumpnand_data* dump = (dumpnand_data*) data;

    view->name = _("Dump NAND");
    view->info = dump->state == DUMP_READY ? _(" start    cancel") : dump->state == DUMP_WORKING ? _(" cancel") : dump->state == DUMP_DONE ? _(" OK") : _(" back");

    if(dump->state == DUMP_WORKING && dump->op.finished) {
        dump->elapsedMs = osGetTime() - dump->startTime;
        if(R_SUCCEEDED(dump->op.result)) {
            unsigned char digest[32];
            mbedtls_sha256_finish_ret(&dump->sha, digest);
            for(u32 i = 0; i < 32; i++) {
                snprintf(dump->hash + i * 2, 3, "%02x", digest[i]);
            }

            dumpnand_write_hash_file(dump);

            dump->state = DUMP_DONE;
            sound_play(SOUND_SUCCESS);
        } else {
            // Cancelled or failed: back to the start, without the half file.
            dumpnand_delete_partial(dump);
            dumpnand_probe(dump);
        }

        mbedtls_sha256_free(&dump->sha);
    }

    u32 down = hidKeysDown();
    if(down & KEY_A) {
        if(dumpnand_act(view, dump, 0)) {
            return;
        }
    } else if(down & KEY_B) {
        if(dumpnand_act(view, dump, dump->state == DUMP_READY ? 1 : 0)) {
            return;
        }
    }

    touchPosition pos;
    hidTouchRead(&pos);
    u32 buttons = dump->state == DUMP_READY ? 2 : 1;
    if(down & KEY_TOUCH) {
        widget_press_end(&dump->press);
        for(u32 i = 0; i < buttons; i++) {
            float x, y, w;
            dumpnand_button_rect(dump, i, bx1, by2, bx2, &x, &y, &w);
            if(widget_touched(&pos, x, y, w, BUTTON_HEIGHT)) {
                widget_press_begin(&dump->press, (int) i, true, pos.px, pos.py);
            }
        }
    } else if((hidKeysHeld() & KEY_TOUCH) && dump->press.index >= 0) {
        float x, y, w;
        dumpnand_button_rect(dump, (u32) dump->press.index, bx1, by2, bx2, &x, &y, &w);
        if(!widget_touched(&pos, x, y, w, BUTTON_HEIGHT)) {
            dump->press.cancelled = true;
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        int index = dump->press.index;
        bool pressed = widget_press_on(&dump->press, index);
        widget_press_end(&dump->press);
        if(pressed && index >= 0) {
            dumpnand_act(view, dump, (u32) index);
        }
    }
}

void dumpnand_open() {
    dumpnand_data* data = (dumpnand_data*) calloc(1, sizeof(dumpnand_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate dump NAND data.");

        return;
    }

    data->press.index = -1;
    data->op.finished = true;
    dumpnand_probe(data);

    ui_view* view = ui_create();
    view->name = _("Dump NAND");
    view->id = "Dump NAND";
    view->info = "";
    view->data = data;
    view->update = dumpnand_update;
    view->drawTop = dumpnand_draw_top;
    view->drawBottom = dumpnand_draw_bottom;
    ui_push(view);
}
