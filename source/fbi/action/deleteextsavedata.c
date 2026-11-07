#include <malloc.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

static Result action_delete_ext_save_data_run(list_item* item) {
    ext_save_data_info* info = (ext_save_data_info*) item->data;
    FS_ExtSaveDataInfo extInfo = {.mediaType = info->mediaType, .saveId = info->extSaveDataId};
    return FSUSER_DeleteExtSaveData(extInfo);
}

static const action_delete_one_def action_delete_ext_save_data_def = {
        .question = "Delete the selected ext save data?",
        .progress = "Deleting Ext Save Data",
        .failure = "Failed to delete ext save data.",
        .success = "Ext save data deleted.",
        .allocError = "Failed to allocate delete ext save data data.",
        .drawInfo = task_draw_ext_save_data_info,
        .run = action_delete_ext_save_data_run,
        .freeItem = task_free_ext_save_data
};

// `items` NULL: `selected` is a copy made for this action, which frees it.
void action_delete_ext_save_data(linked_list* items, list_item* selected) {
    action_delete_one(&action_delete_ext_save_data_def, items, selected);
}
