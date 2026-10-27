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

static void task_populate_titles_load_ctr(list_item* item) {
    title_info* titleInfo = (title_info*) item->data;
    FS_MediaType mediaType = titleInfo->mediaType;
    u64 titleId = titleInfo->titleId;

    AM_TitleEntry entry;
    if(R_SUCCEEDED(AM_GetTitleInfo(mediaType, 1, &titleId, &entry))) {
        AM_GetTitleProductCode(mediaType, titleId, titleInfo->productCode);
        titleInfo->version = entry.version;
        titleInfo->installedSize = entry.size;
    }

    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) ((titleId >> 32) & 0xFFFFFFFF), mediaType, 0x00000000};

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

                utf16_to_utf8((uint8_t*) titleInfo->meta.shortDescription, smdhTitle->shortDescription, sizeof(titleInfo->meta.shortDescription) - 1);
                utf16_to_utf8((uint8_t*) titleInfo->meta.longDescription, smdhTitle->longDescription, sizeof(titleInfo->meta.longDescription) - 1);
                utf16_to_utf8((uint8_t*) titleInfo->meta.publisher, smdhTitle->publisher, sizeof(titleInfo->meta.publisher) - 1);
                titleInfo->meta.region = smdh->region;
                titleInfo->meta.texture = screen_allocate_free_texture();
                screen_load_texture_tiled(titleInfo->meta.texture, smdh->largeIcon, sizeof(smdh->largeIcon), 48, 48, GPU_RGB565, false);
                titleInfo->hasMeta = true;

                if(!string_is_empty(titleInfo->meta.shortDescription)) {
                    string_copy(item->name, titleInfo->meta.shortDescription, LIST_ITEM_NAME_MAX);
                }
            }

            free(smdh);
        }

        FSFILE_Close(fileHandle);
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

static Result task_populate_titles_from(populate_titles_data* data, FS_MediaType mediaType, bool useDSiWare) {
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

                    for(u32 i = 0; i < titleCount && R_SUCCEEDED(res); i++) {
                        svcWaitSynchronization(task_get_pause_event(), U64_MAX);
                        if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                            break;
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

                            linked_list_add(data->items, item);
                        }
                    }
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

    if(R_SUCCEEDED(res = task_populate_titles_from(data, MEDIATYPE_GAME_CARD, false))) {
        if(R_SUCCEEDED(res = task_populate_titles_from(data, MEDIATYPE_SD, false))) {
            if(R_SUCCEEDED(res = task_populate_titles_from(data, MEDIATYPE_NAND, false))) {
                res = task_populate_titles_from(data, MEDIATYPE_NAND, true);
            }
        }
    }

    // Then each title's details, in list order.
    for(u32 i = 0; R_SUCCEEDED(res) && i < linked_list_size(data->items); i++) {
        svcWaitSynchronization(task_get_pause_event(), U64_MAX);
        if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
            break;
        }

        list_item* item = (list_item*) linked_list_get(data->items, i);
        if(item == NULL || ((title_info*) item->data)->loaded) {
            continue;
        }

        if(((title_info*) item->data)->twl) {
            task_populate_titles_load_twl(item);
        } else {
            task_populate_titles_load_ctr(item);
        }
    }

    svcCloseHandle(data->cancelEvent);

    data->result = res;
    data->finished = true;
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
    if(items == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);

        linked_list_iter_remove(&iter);
        task_free_title(item);
    }
}

Result task_populate_titles(populate_titles_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_titles(data->items);

    data->finished = false;
    data->result = 0;
    data->cancelEvent = 0;

    Result res = 0;
    if(R_SUCCEEDED(res = svcCreateEvent(&data->cancelEvent, RESET_STICKY))) {
        if(threadCreate(task_populate_titles_thread, data, 0x10000, 0x19, 1, true) == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
        }
    }

    if(R_FAILED(res)) {
        data->finished = true;

        if(data->cancelEvent != 0) {
            svcCloseHandle(data->cancelEvent);
            data->cancelEvent = 0;
        }
    }

    return res;
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
