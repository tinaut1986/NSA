#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "uitask.h"
#include "listtitles.h"
#include "../resources.h"
#include "../../core/core.h"

// Titles are listed in two passes: first a placeholder for every title (its
// ID is all that is known), so the list has its final length at once; then
// each one's details, filled in place. The list sorts itself once at the end
// (titles.c), so rows do not move around while they load.

// A cache of what each CTR title's SMDH holds (name, publisher, region, icon),
// kept in sdmc:/nsa/titles-cache.bin. Opening an SMDH for every title is what
// makes a long list slow to fill; a title whose ID and version are unchanged
// is taken from here instead. Only the populate thread touches it. The icons
// stay on the SD (the app has little memory to spare): only a small index is
// kept, and the records are read one at a time.
#define TITLE_CACHE_PATH "sdmc:/nsa/titles-cache.bin"
#define TITLE_CACHE_NEW_PATH "sdmc:/nsa/titles-cache.new"
#define TITLE_CACHE_TMP_PATH "sdmc:/nsa/titles-cache.tmp"
#define TITLE_CACHE_MAGIC 0x5441534E // "NSAT"
#define TITLE_CACHE_FORMAT 4
#define TITLE_CACHE_HEADER_SIZE 12
#define TITLE_CACHE_MAX 2048

// The first five fields are the key, version, flags and product code: the
// index is built by reading just those (TITLE_CACHE_KEY_SIZE bytes) from each record.
#define TITLE_CACHE_KEY_SIZE 36
typedef struct title_cache_record_s {
    u64 titleId;
    u32 mediaType;
    u32 version;
    u32 flags; // TITLE_CACHE_NO_META: it has no SMDH (updates, DLC...), so there is nothing to read
    char productCode[0x10];
    u32 region;
    char shortDescription[0x100];
    char longDescription[0x200];
    char publisher[0x100];
    u16 icon[48 * 48];
} title_cache_record;

#define TITLE_CACHE_NO_META 1

typedef struct title_cache_entry_s {
    u64 titleId;
    u32 mediaType;
    u32 version;
    u32 flags;
    char productCode[0x10];
    u32 offset;    // of the record in the file it is in
    bool inNew;    // in the file of records made in this pass, not in the old one
    bool present;  // still installed in this pass
} title_cache_entry;

typedef struct title_cache_s {
    title_cache_entry* entries;
    u32 count;
    FILE* old;     // the cache on the SD, as it was
    FILE* added;   // records read from SMDHs in this pass
    u32 addedCount;
    bool dirty;
    u32 hits;
    u32 misses;
} title_cache;

static title_cache_entry* title_cache_find(title_cache* cache, FS_MediaType mediaType, u64 titleId) {
    for(u32 i = 0; i < cache->count; i++) {
        if(cache->entries[i].titleId == titleId && cache->entries[i].mediaType == (u32) mediaType) {
            return &cache->entries[i];
        }
    }

    return NULL;
}

static bool title_cache_read(title_cache* cache, const title_cache_entry* entry, title_cache_record* rec) {
    FILE* f = entry->inNew ? cache->added : cache->old;
    return f != NULL && fseek(f, entry->offset, SEEK_SET) == 0 && fread(rec, sizeof(*rec), 1, f) == 1;
}

// `refresh` ignores what is on the SD (it is rebuilt from the SMDHs).
static title_cache* title_cache_load(bool refresh) {
    title_cache* cache = (title_cache*) calloc(1, sizeof(title_cache));
    if(cache == NULL) {
        return NULL;
    }

    cache->entries = (title_cache_entry*) calloc(TITLE_CACHE_MAX, sizeof(title_cache_entry));
    if(cache->entries == NULL) {
        free(cache);
        return NULL;
    }

    if(refresh) {
        cache->dirty = true;
        return cache;
    }

    cache->old = fopen(TITLE_CACHE_PATH, "rb");
    if(cache->old != NULL) {
        u32 header[3] = {0, 0, 0};
        if(fread(header, sizeof(header), 1, cache->old) == 1 && header[0] == TITLE_CACHE_MAGIC && header[1] == TITLE_CACHE_FORMAT && header[2] <= TITLE_CACHE_MAX) {
            for(u32 i = 0; i < header[2]; i++) {
                u32 offset = TITLE_CACHE_HEADER_SIZE + i * (u32) sizeof(title_cache_record);
                u8 key[TITLE_CACHE_KEY_SIZE];
                if(fseek(cache->old, offset, SEEK_SET) != 0 || fread(key, sizeof(key), 1, cache->old) != 1) {
                    // Truncated: keep what was read whole.
                    cache->dirty = true;
                    break;
                }

                title_cache_entry* entry = &cache->entries[cache->count++];
                memcpy(&entry->titleId, key, 8);
                memcpy(&entry->mediaType, key + 8, 4);
                memcpy(&entry->version, key + 12, 4);
                memcpy(&entry->flags, key + 16, 4);
                memcpy(entry->productCode, key + 20, 0x10);
                entry->offset = offset;
            }
        } else {
            cache->dirty = true;
        }
    }

    return cache;
}

static void title_cache_free(title_cache* cache) {
    if(cache == NULL) {
        return;
    }

    if(cache->old != NULL) {
        fclose(cache->old);
    }

    if(cache->added != NULL) {
        fclose(cache->added);
        remove(TITLE_CACHE_NEW_PATH);
    }

    free(cache->entries);
    free(cache);
}

// Writes the cache when it changed, then frees it. `complete` says the pass saw
// every installed title, so the ones not seen are gone and are dropped.
static void title_cache_finish(title_cache* cache, bool complete) {
    if(cache == NULL) {
        return;
    }

    if(complete) {
        u32 kept = 0;
        for(u32 i = 0; i < cache->count; i++) {
            if(cache->entries[i].present) {
                cache->entries[kept++] = cache->entries[i];
            } else {
                cache->dirty = true;
            }
        }

        cache->count = kept;
    }

    title_cache_record* rec = cache->dirty ? (title_cache_record*) malloc(sizeof(title_cache_record)) : NULL;
    FILE* f = rec != NULL ? fopen(TITLE_CACHE_TMP_PATH, "wb") : NULL;
    if(f != NULL) {
        u32 header[3] = {TITLE_CACHE_MAGIC, TITLE_CACHE_FORMAT, cache->count};
        bool ok = fwrite(header, sizeof(header), 1, f) == 1;
        for(u32 i = 0; ok && i < cache->count; i++) {
            ok = title_cache_read(cache, &cache->entries[i], rec) && fwrite(rec, sizeof(*rec), 1, f) == 1;
        }

        ok = fclose(f) == 0 && ok;
        if(ok) {
            if(cache->old != NULL) {
                fclose(cache->old);
                cache->old = NULL;
            }

            remove(TITLE_CACHE_PATH);
            ok = rename(TITLE_CACHE_TMP_PATH, TITLE_CACHE_PATH) == 0;
        }

        if(!ok) {
            remove(TITLE_CACHE_TMP_PATH);
        }
    }

    free(rec);
    title_cache_free(cache);
}

// Adds (or replaces) a title read from its SMDH.
// `meta` and `icon` are NULL for a title without an SMDH.
static void title_cache_store(title_cache* cache, FS_MediaType mediaType, u64 titleId, u32 version, const char* productCode, const meta_info* meta, const u16* icon) {
    title_cache_record* rec = (title_cache_record*) calloc(1, sizeof(title_cache_record));
    if(rec == NULL) {
        return;
    }

    if(cache->added == NULL) {
        cache->added = fopen(TITLE_CACHE_NEW_PATH, "w+b");
    }

    title_cache_entry* entry = title_cache_find(cache, mediaType, titleId);
    if(entry == NULL && cache->count < TITLE_CACHE_MAX) {
        entry = &cache->entries[cache->count++];
    }

    if(cache->added != NULL && entry != NULL) {
        rec->titleId = titleId;
        rec->mediaType = mediaType;
        rec->version = version;
        memcpy(rec->productCode, productCode, sizeof(rec->productCode));
        if(meta != NULL && icon != NULL) {
            rec->region = meta->region;
            memcpy(rec->shortDescription, meta->shortDescription, sizeof(rec->shortDescription));
            memcpy(rec->longDescription, meta->longDescription, sizeof(rec->longDescription));
            memcpy(rec->publisher, meta->publisher, sizeof(rec->publisher));
            memcpy(rec->icon, icon, sizeof(rec->icon));
        } else {
            rec->flags = TITLE_CACHE_NO_META;
        }

        u32 offset = cache->addedCount * (u32) sizeof(*rec);
        if(fseek(cache->added, offset, SEEK_SET) == 0 && fwrite(rec, sizeof(*rec), 1, cache->added) == 1) {
            entry->titleId = titleId;
            entry->mediaType = mediaType;
            entry->version = version;
            entry->flags = rec->flags;
            memcpy(entry->productCode, rec->productCode, sizeof(entry->productCode));
            entry->offset = offset;
            entry->inNew = true;
            entry->present = true;
            cache->addedCount++;
            cache->dirty = true;
        } else if(entry == &cache->entries[cache->count - 1] && entry->titleId == 0) {
            cache->count--;
        }
    }

    free(rec);
}

// Fills `meta` from a cached record; the icon texture belongs to the caller.
static void title_cache_apply(title_cache_record* rec, meta_info* meta) {
    memcpy(meta->shortDescription, rec->shortDescription, sizeof(meta->shortDescription));
    memcpy(meta->longDescription, rec->longDescription, sizeof(meta->longDescription));
    memcpy(meta->publisher, rec->publisher, sizeof(meta->publisher));
    meta->region = rec->region;
    meta->texture = screen_allocate_free_texture();
    screen_load_texture_tiled(meta->texture, rec->icon, sizeof(rec->icon), 48, 48, GPU_RGB565, false);
}

static list_item* task_populate_titles_placeholder(FS_MediaType mediaType, u64 titleId, bool twl) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    title_info* titleInfo = (title_info*) calloc(1, sizeof(title_info));
    if(item == NULL || titleInfo == NULL) {
        free(item);
        free(titleInfo);
        return NULL;
    }

    titleInfo->mediaType = mediaType;
    titleInfo->titleId = titleId;
    titleInfo->twl = twl;

    snprintf(item->name, LIST_ITEM_NAME_MAX, "%016llX", titleId);
    item->color = twl ? COLOR_DS_TITLE : mediaType == MEDIATYPE_NAND ? COLOR_NAND : mediaType == MEDIATYPE_SD ? COLOR_SD : COLOR_GAME_CARD;
    item->data = titleInfo;
    return item;
}

// Marks a title loaded once everything it shows is in place (the UI reads it meanwhile).
static void task_populate_titles_loaded(title_info* titleInfo) {
    titleInfo->loadedTime = osGetTime();
    titleInfo->loaded = true;
}

// A CTR title's banner (name, publisher, region, icon) from its SMDH; the icon
// texture is allocated here and belongs to the caller. `iconOut` (optional)
// gets the raw 48x48 icon too, for the cache.
static bool task_title_load_meta_icon(FS_MediaType mediaType, u64 titleId, meta_info* meta, u16* iconOut) {
    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) ((titleId >> 32) & 0xFFFFFFFF), mediaType, 0x00000000};

    bool loaded = false;
    Handle fileHandle;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SAVEDATA_AND_CONTENT,
                                           fs_make_path_binary(archivePath, sizeof(archivePath)),
                                           fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
        if(smdh != NULL) {
            u32 bytesRead = 0;
            if(R_SUCCEEDED(FSFILE_Read(fileHandle, &bytesRead, 0, smdh, sizeof(SMDH))) && bytesRead == sizeof(SMDH)
               && smdh->magic[0] == 'S' && smdh->magic[1] == 'M' && smdh->magic[2] == 'D' && smdh->magic[3] == 'H') {
                SMDH_title* smdhTitle = smdh_select_title(smdh);

                utf16_to_utf8((uint8_t*) meta->shortDescription, smdhTitle->shortDescription, sizeof(meta->shortDescription) - 1);
                utf16_to_utf8((uint8_t*) meta->longDescription, smdhTitle->longDescription, sizeof(meta->longDescription) - 1);
                utf16_to_utf8((uint8_t*) meta->publisher, smdhTitle->publisher, sizeof(meta->publisher) - 1);
                meta->region = smdh->region;
                meta->texture = screen_allocate_free_texture();
                screen_load_texture_tiled(meta->texture, smdh->largeIcon, sizeof(smdh->largeIcon), 48, 48, GPU_RGB565, false);
                if(iconOut != NULL) {
                    memcpy(iconOut, smdh->largeIcon, sizeof(smdh->largeIcon));
                }

                loaded = true;
            }

            free(smdh);
        }

        FSFILE_Close(fileHandle);
    }

    return loaded;
}

bool task_title_load_meta(FS_MediaType mediaType, u64 titleId, meta_info* meta) {
    return task_title_load_meta_icon(mediaType, titleId, meta, NULL);
}

// `cache` (optional) spares opening the SMDH of a title that has not changed.
#if NSA_DEBUG
static u64 dbg_am_ms, dbg_cache_ms, dbg_smdh_ms, dbg_tex_ms;
#define DBG_T(v) u64 v = osGetTime()
#define DBG_ADD(acc, v) acc += osGetTime() - v
#else
#define DBG_T(v)
#define DBG_ADD(acc, v)
#endif

static void task_populate_titles_load_ctr(list_item* item, title_cache* cache) {
    title_info* titleInfo = (title_info*) item->data;
    FS_MediaType mediaType = titleInfo->mediaType;
    u64 titleId = titleInfo->titleId;

    DBG_T(t0);
    // Version and size come from one call for the whole list when it worked (task_populate_titles_from).
    bool known = titleInfo->infoKnown;
    AM_TitleEntry entry;
    if(!known && R_SUCCEEDED(AM_GetTitleInfo(mediaType, 1, &titleId, &entry))) {
        titleInfo->version = entry.version;
        titleInfo->installedSize = entry.size;
        known = true;
    }

    DBG_ADD(dbg_am_ms, t0);
    DBG_T(t1);
    bool hasMeta = false;
    bool cacheHit = false;
    title_cache_entry* cached = cache != NULL && known ? title_cache_find(cache, mediaType, titleId) : NULL;
    if(cached != NULL && cached->version == titleInfo->version && (cached->flags & TITLE_CACHE_NO_META) != 0) {
        memcpy(titleInfo->productCode, cached->productCode, sizeof(titleInfo->productCode));
        cached->present = true;
        cache->hits++;
        cacheHit = true;
    } else if(cached != NULL && cached->version == titleInfo->version) {
        title_cache_record* rec = (title_cache_record*) malloc(sizeof(title_cache_record));
        if(rec != NULL) {
            if(title_cache_read(cache, cached, rec)) {
                DBG_ADD(dbg_cache_ms, t1);
                memcpy(titleInfo->productCode, rec->productCode, sizeof(titleInfo->productCode));
                DBG_T(t2);
                title_cache_apply(rec, &titleInfo->meta);
                DBG_ADD(dbg_tex_ms, t2);
                hasMeta = true;

                cached->present = true;
                cache->hits++;
                cacheHit = true;
            }

            free(rec);
        }
    }

    if(!cacheHit && cache != NULL && known) {
        u16* icon = (u16*) malloc(sizeof(((title_cache_record*) NULL)->icon));
        if(icon != NULL) {
            AM_GetTitleProductCode(mediaType, titleId, titleInfo->productCode);
            hasMeta = task_title_load_meta_icon(mediaType, titleId, &titleInfo->meta, icon);
            DBG_ADD(dbg_smdh_ms, t1);
            title_cache_store(cache, mediaType, titleId, titleInfo->version, titleInfo->productCode, hasMeta ? &titleInfo->meta : NULL, hasMeta ? icon : NULL);

            free(icon);
        }

        cache->misses++;
    } else if(!cacheHit) {
        if(known) {
            AM_GetTitleProductCode(mediaType, titleId, titleInfo->productCode);
        }

        hasMeta = task_title_load_meta(mediaType, titleId, &titleInfo->meta);
    }

    if(hasMeta) {
        titleInfo->hasMeta = true;
        if(!string_is_empty(titleInfo->meta.shortDescription)) {
            string_copy(item->name, titleInfo->meta.shortDescription, LIST_ITEM_NAME_MAX);
        }
    }

    task_populate_titles_loaded(titleInfo);
}

static void task_populate_titles_load_twl(list_item* item) {
    title_info* titleInfo = (title_info*) item->data;
    FS_MediaType mediaType = titleInfo->mediaType;
    u64 titleId = titleInfo->titleId;

    u8 header[0x3B4] = {0};
    Result headerRes = FSUSER_GetLegacyRomHeader(mediaType, titleId, header);

    AM_TitleEntry entry;
    if(R_SUCCEEDED(AM_GetTitleInfo(mediaType, 1, &titleId, &entry))) {
        AM_GetTitleProductCode(mediaType, titleId, titleInfo->productCode);
        titleInfo->version = entry.version;
        titleInfo->installedSize = entry.size;
    } else if(R_SUCCEEDED(headerRes)) {
        // A DS game card: what its header says.
        u64 realTitleId = 0;
        memcpy(&realTitleId, &header[0x230], sizeof(realTitleId));
        titleInfo->titleId = realTitleId;
        memcpy(titleInfo->productCode, header, 0xC);
        titleInfo->version = header[0x01E];

        u32 size = 0;
        if((header[0x012] & 0x2) != 0) {
            memcpy(&size, &header[0x210], sizeof(size));
        } else {
            memcpy(&size, &header[0x080], sizeof(size));
        }

        titleInfo->installedSize = size;
    }

    char name[LIST_ITEM_NAME_MAX] = {'\0'};
    meta_info meta;
    memset(&meta, 0, sizeof(meta));
    bool hasMeta = false;

    BNR* bnr = (BNR*) calloc(1, sizeof(BNR));
    if(bnr != NULL) {
        if(R_SUCCEEDED(FSUSER_GetLegacyBannerData(mediaType, titleId, (u8*) bnr))) {
            hasMeta = true;

            char title[0x100] = {'\0'};
            utf16_to_utf8((uint8_t*) title, bnr_select_title(bnr), sizeof(title) - 1);

            if(strchr(title, '\n') == NULL) {
                string_copy(name, title, sizeof(name));
                string_copy(meta.shortDescription, title, sizeof(meta.shortDescription));
            } else {
                char* destinations[] = {meta.shortDescription, meta.longDescription, meta.publisher};
                u32 destinationLens[] = {sizeof(meta.shortDescription), sizeof(meta.longDescription), sizeof(meta.publisher)};
                int currDest = 0;

                char* last = title;
                char* curr = NULL;

                while(currDest < 3 && (curr = strchr(last, '\n')) != NULL) {
                    u32 copyLen = curr - last + 1;
                    if(copyLen > destinationLens[currDest]) {
                        copyLen = destinationLens[currDest];
                    }

                    string_copy(destinations[currDest++], last, copyLen);

                    last = curr + 1;
                    *curr = ' ';
                }

                string_copy(name, title, last - title);
                if(currDest < 3) {
                    string_copy(destinations[currDest], last, destinationLens[currDest]);
                }
            }

            u8 icon[32 * 32 * 2];
            for(u32 x = 0; x < 32; x++) {
                for(u32 y = 0; y < 32; y++) {
                    u32 srcPos = (((y >> 3) * 4 + (x >> 3)) * 8 + (y & 7)) * 4 + ((x & 7) >> 1);
                    u32 srcShift = (x & 1) * 4;
                    u16 srcPx = bnr->mainIconPalette[(bnr->mainIconBitmap[srcPos] >> srcShift) & 0xF];

                    u8 r = (u8) (srcPx & 0x1F);
                    u8 g = (u8) ((srcPx >> 5) & 0x1F);
                    u8 b = (u8) ((srcPx >> 10) & 0x1F);

                    u16 reversedPx = (u16) ((r << 11) | (g << 6) | (b << 1) | 1);

                    u32 dstPos = (y * 32 + x) * 2;
                    icon[dstPos + 0] = (u8) (reversedPx & 0xFF);
                    icon[dstPos + 1] = (u8) ((reversedPx >> 8) & 0xFF);
                }
            }

            if(R_SUCCEEDED(headerRes)) {
                memcpy(&meta.region, &header[0x1B0], sizeof(meta.region));
            } else {
                meta.region = 0;
            }

            meta.texture = screen_allocate_free_texture();
            screen_load_texture_untiled(meta.texture, icon, sizeof(icon), 32, 32, GPU_RGBA5551, false);
        }

        free(bnr);
    }

    if(hasMeta) {
        titleInfo->meta = meta;
        titleInfo->hasMeta = true;
    }

    if(!string_is_empty(name)) {
        string_copy(item->name, name, LIST_ITEM_NAME_MAX);
    } else {
        snprintf(item->name, LIST_ITEM_NAME_MAX, "%016llX", titleInfo->titleId);
    }

    task_populate_titles_loaded(titleInfo);
}

static int task_populate_titles_compare_ids(const void* e1, const void* e2) {
    u64 id1 = *(u64*) e1;
    u64 id2 = *(u64*) e2;

    return id1 > id2 ? 1 : id1 < id2 ? -1 : 0;
}

static Result task_populate_titles_from(populate_titles_data* data, title_cache* cache, FS_MediaType mediaType, bool useDSiWare) {
    bool inserted;
    FS_CardType type;
    if(mediaType == MEDIATYPE_GAME_CARD && (R_FAILED(FSUSER_CardSlotIsInserted(&inserted)) || !inserted || R_FAILED(FSUSER_GetCardType(&type)))) {
        return 0;
    }

    Result res = 0;

    if(mediaType != MEDIATYPE_GAME_CARD || type == CARD_CTR) {
        u32 titleCount = 0;
        if(R_SUCCEEDED(res = AM_GetTitleCount(mediaType, &titleCount))) {
            u64* titleIds = (u64*) calloc(titleCount, sizeof(u64));
            if(titleIds != NULL) {
                if(R_SUCCEEDED(res = AM_GetTitleList(&titleCount, mediaType, titleCount, titleIds))) {
                    qsort(titleIds, titleCount, sizeof(u64), task_populate_titles_compare_ids);

                    // Version and size of every title in one call instead of one each.
                    AM_TitleEntry* infos = !useDSiWare && titleCount > 0 ? (AM_TitleEntry*) calloc(titleCount, sizeof(AM_TitleEntry)) : NULL;
                    if(infos != NULL && R_FAILED(AM_GetTitleInfo(mediaType, titleCount, titleIds, infos))) {
                        free(infos);
                        infos = NULL;
                    }

                    for(u32 i = 0; i < titleCount && R_SUCCEEDED(res); i++) {
                        svcWaitSynchronization(task_get_pause_event(), U64_MAX);
                        if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                            break;
                        }

                        // Installed, whether or not the list shows it: its cache entry stays.
                        if(cache != NULL) {
                            title_cache_entry* cached = title_cache_find(cache, mediaType, titleIds[i]);
                            if(cached != NULL) {
                                cached->present = true;
                            }
                        }

                        if(data->filter == NULL || data->filter(data->userData, titleIds[i], mediaType)) {
                            bool dsiWare = ((titleIds[i] >> 32) & 0x8000) != 0;
                            if(dsiWare != useDSiWare) {
                                continue;
                            }

                            list_item* item = task_populate_titles_placeholder(mediaType, titleIds[i], dsiWare);
                            if(item == NULL) {
                                res = R_APP_OUT_OF_MEMORY;
                                break;
                            }

                            if(infos != NULL) {
                                title_info* info = (title_info*) item->data;
                                info->version = infos[i].version;
                                info->installedSize = infos[i].size;
                                info->infoKnown = true;
                            }

                            linked_list_add(data->items, item);
                        }
                    }

                    free(infos);
                }

                free(titleIds);
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }
    } else {
        list_item* item = task_populate_titles_placeholder(mediaType, 0, true);
        if(item != NULL) {
            linked_list_add(data->items, item);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    return res;
}

static void task_populate_titles_thread(void* arg) {
    populate_titles_data* data = (populate_titles_data*) arg;

    Result res = 0;

    u64 started = osGetTime();
    title_cache* cache = title_cache_load(data->refresh);

    if(R_SUCCEEDED(res = task_populate_titles_from(data, cache, MEDIATYPE_GAME_CARD, false))) {
        if(R_SUCCEEDED(res = task_populate_titles_from(data, cache, MEDIATYPE_SD, false))) {
            if(R_SUCCEEDED(res = task_populate_titles_from(data, cache, MEDIATYPE_NAND, false))) {
                res = task_populate_titles_from(data, cache, MEDIATYPE_NAND, true);
            }
        }
    }

    u64 listed = osGetTime();

    // Then each title's details. The UI sorts the list as they come, which
    // moves items between passes: go over it again until none is missing.
    bool missing = true;
    bool quit = false;
    while(R_SUCCEEDED(res) && missing && !quit) {
        missing = false;
        for(u32 i = 0; i < linked_list_size(data->items); i++) {
            svcWaitSynchronization(task_get_pause_event(), U64_MAX);
            if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                quit = true;
                break;
            }

            list_item* item = (list_item*) linked_list_get(data->items, i);
            if(item == NULL || ((title_info*) item->data)->loaded) {
                continue;
            }

            missing = true;
            if(((title_info*) item->data)->twl) {
                task_populate_titles_load_twl(item);
            } else {
                task_populate_titles_load_ctr(item, cache);
            }
        }
    }

    u32 hits = cache != NULL ? cache->hits : 0;
    u32 misses = cache != NULL ? cache->misses : 0;
    if(cache != NULL) {
        // Only a pass that went through every title may drop the ones not seen.
        title_cache_finish(cache, R_SUCCEEDED(res) && !quit && !missing);

#if NSA_DEBUG
        FILE* log = fopen("sdmc:/nsa/titles-log.txt", "a");
        if(log != NULL) {
            fprintf(log, "%s: %lu titles listed in %lu ms, details in %lu ms (%lu from cache, %lu read)\n", data->refresh ? "refresh" : "load",
                    (u32) linked_list_size(data->items), (u32) (listed - started), (u32) (osGetTime() - listed), hits, misses);
            fprintf(log, "  AM %lu ms, cache reads %lu ms, textures %lu ms, SMDH %lu ms\n", (u32) dbg_am_ms, (u32) dbg_cache_ms, (u32) dbg_tex_ms, (u32) dbg_smdh_ms);
            dbg_am_ms = dbg_cache_ms = dbg_smdh_ms = dbg_tex_ms = 0;
            fclose(log);
        }
#endif

    }

    svcCloseHandle(data->cancelEvent);

    data->result = res;
    data->finished = true;
}

list_item* task_title_item(FS_MediaType mediaType, u64 titleId) {
    list_item* item = task_populate_titles_placeholder(mediaType, titleId, false);
    if(item != NULL) {
        task_populate_titles_load_ctr(item, NULL);
    }

    return item;
}

void task_free_title(list_item* item) {
    if(item == NULL) {
        return;
    }

    if(item->data != NULL) {
        title_info* titleInfo = (title_info*) item->data;
        if(titleInfo->hasMeta) {
            screen_unload_texture(titleInfo->meta.texture);
        }

        free(item->data);
    }

    free(item);
}

void task_clear_titles(linked_list* items) {
    task_clear_list(items, task_free_title);
}

Result task_populate_titles(populate_titles_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_titles(data->items);

    return task_start_populate(task_populate_titles_thread, data, &data->cancelEvent, &data->finished, &data->result);
}

title_kind title_kind_of(u64 titleId) {
    u32 high = (u32) (titleId >> 32);
    switch(high) {
        case 0x00040000: return TITLE_KIND_GAME;
        case 0x0004000E: return TITLE_KIND_UPDATE;
        case 0x0004008C: return TITLE_KIND_DLC;
        case 0x00040002: return TITLE_KIND_DEMO;
        default: return (high & 0x00008000) != 0 ? TITLE_KIND_DSIWARE : TITLE_KIND_SYSTEM;
    }
}
