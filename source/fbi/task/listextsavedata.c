#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "uitask.h"
#include "listextsavedata.h"
#include "../resources.h"
#include "../../core/core.h"

#define MAX_EXT_SAVE_DATA 512

static int task_populate_ext_save_data_compare_ids(const void* e1, const void* e2) {
    u64 id1 = *(u64*) e1;
    u64 id2 = *(u64*) e2;

    return id1 > id2 ? 1 : id1 < id2 ? -1 : 0;
}

// An ext save data's name and icon, from the icon file it keeps.
void task_ext_save_data_load(list_item* item) {
    ext_save_data_info* info = (ext_save_data_info*) item->data;
    FS_ExtSaveDataInfo saveInfo = {.mediaType = info->mediaType, .saveId = info->extSaveDataId};

    SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
    if(smdh != NULL) {
        u32 smdhBytesRead = 0;
        if(R_SUCCEEDED(FSUSER_ReadExtSaveDataIcon(&smdhBytesRead, saveInfo, sizeof(SMDH), (u8*) smdh)) && smdhBytesRead == sizeof(SMDH)
           && smdh->magic[0] == 'S' && smdh->magic[1] == 'M' && smdh->magic[2] == 'D' && smdh->magic[3] == 'H') {
            SMDH_title* smdhTitle = smdh_select_title(smdh);

            utf16_to_utf8((uint8_t*) info->meta.shortDescription, smdhTitle->shortDescription, sizeof(info->meta.shortDescription) - 1);
            utf16_to_utf8((uint8_t*) info->meta.longDescription, smdhTitle->longDescription, sizeof(info->meta.longDescription) - 1);
            utf16_to_utf8((uint8_t*) info->meta.publisher, smdhTitle->publisher, sizeof(info->meta.publisher) - 1);
            info->meta.region = smdh->region;
            info->meta.texture = screen_allocate_free_texture();
            screen_load_texture_tiled(info->meta.texture, smdh->largeIcon, sizeof(smdh->largeIcon), 48, 48, GPU_RGB565, false);
            info->hasMeta = true;

            if(!string_is_empty(info->meta.shortDescription)) {
                string_copy(item->name, info->meta.shortDescription, LIST_ITEM_NAME_MAX);
            }
        }

        free(smdh);
    }

    info->readyTime = osGetTime();
    info->ready = true;
}

// Ext save data is listed in two passes: first a placeholder for every one
// (its ID is all that is known), then each one's name and icon, filled in
// place. The screen sorts the list as they come.

static Result task_populate_ext_save_data_from(populate_ext_save_data_data* data, FS_MediaType mediaType) {
    Result res = 0;

    u32 extSaveDataCount = 0;
    u64 extSaveDataIds[MAX_EXT_SAVE_DATA];
    if(R_SUCCEEDED(res = FSUSER_EnumerateExtSaveData(&extSaveDataCount, MAX_EXT_SAVE_DATA, mediaType, 8, mediaType == MEDIATYPE_NAND, (u8*) extSaveDataIds))) {
        qsort(extSaveDataIds, extSaveDataCount, sizeof(u64), task_populate_ext_save_data_compare_ids);

        for(u32 i = 0; i < extSaveDataCount && R_SUCCEEDED(res); i++) {
            svcWaitSynchronization(task_get_pause_event(), U64_MAX);
            if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                break;
            }

            if(data->filter == NULL || data->filter(data->userData, extSaveDataIds[i], mediaType)) {
                list_item* item = (list_item*) calloc(1, sizeof(list_item));
                ext_save_data_info* extSaveDataInfo = (ext_save_data_info*) calloc(1, sizeof(ext_save_data_info));
                if(item == NULL || extSaveDataInfo == NULL) {
                    free(item);
                    free(extSaveDataInfo);
                    res = R_APP_OUT_OF_MEMORY;
                    break;
                }

                extSaveDataInfo->mediaType = mediaType;
                extSaveDataInfo->extSaveDataId = extSaveDataIds[i];
                extSaveDataInfo->shared = mediaType == MEDIATYPE_NAND;
                extSaveDataInfo->hasMeta = false;
                extSaveDataInfo->ready = !data->placeholders;

                snprintf(item->name, LIST_ITEM_NAME_MAX, "%016llX", extSaveDataIds[i]);
                item->color = mediaType == MEDIATYPE_NAND ? COLOR_NAND : COLOR_SD;
                item->data = extSaveDataInfo;

                if(data->placeholders) {
                    linked_list_add(data->items, item);
                } else {
                    task_ext_save_data_load(item);
                    linked_list_add_sorted(data->items, item, data->userData, data->compare);
                }
            }
        }
    }

    return res;
}

static void task_populate_ext_save_data_thread(void* arg) {
    populate_ext_save_data_data* data = (populate_ext_save_data_data*) arg;

    Result res = 0;

    if(R_SUCCEEDED(res = task_populate_ext_save_data_from(data, MEDIATYPE_SD))) {
        res = task_populate_ext_save_data_from(data, MEDIATYPE_NAND);
    }

    // Then each one's name and icon. The screen sorts the list as they come,
    // which moves rows between passes: go over it again until none is missing.
    bool missing = data->placeholders;
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
            if(item == NULL || ((ext_save_data_info*) item->data)->ready) {
                continue;
            }

            missing = true;
            task_ext_save_data_load(item);
        }
    }

    svcCloseHandle(data->cancelEvent);

    data->result = res;
    data->finished = true;
}

void task_free_ext_save_data(list_item* item) {
    if(item == NULL) {
        return;
    }

    if(item->data != NULL) {
        ext_save_data_info* extSaveDataInfo = (ext_save_data_info*) item->data;
        if(extSaveDataInfo->hasMeta) {
            screen_unload_texture(extSaveDataInfo->meta.texture);
        }

        if(extSaveDataInfo->hasTitleMeta) {
            screen_unload_texture(extSaveDataInfo->titleMeta.texture);
        }

        free(item->data);
    }

    free(item);
}

void task_clear_ext_save_data(linked_list* items) {
    if(items == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);

        linked_list_iter_remove(&iter);
        task_free_ext_save_data(item);
    }
}

Result task_populate_ext_save_data(populate_ext_save_data_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_ext_save_data(data->items);

    data->finished = false;
    data->result = 0;
    data->cancelEvent = 0;

    Result res = 0;
    if(R_SUCCEEDED(res = svcCreateEvent(&data->cancelEvent, RESET_STICKY))) {
        if(threadCreate(task_populate_ext_save_data_thread, data, 0x10000, 0x19, 1, true) == NULL) {
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