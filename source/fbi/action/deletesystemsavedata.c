#include <malloc.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

static Result action_delete_system_save_data_run(list_item* item) {
    system_save_data_info* info = (system_save_data_info*) item->data;
    FS_SystemSaveDataInfo sysInfo = {.mediaType = MEDIATYPE_NAND, .saveId = info->systemSaveDataId};
    return FSUSER_DeleteSystemSaveData(sysInfo);
}

static const action_delete_one_def action_delete_system_save_data_def = {
        .question = "Delete the selected system save data?",
        .progress = "Deleting System Save Data",
        .failure = "Failed to delete system save data.",
        .success = "System save data deleted.",
        .allocError = "Failed to allocate delete system save data data.",
        .drawInfo = task_draw_system_save_data_info,
        .run = action_delete_system_save_data_run,
        .freeItem = task_free_system_save_data
};

void action_delete_system_save_data(linked_list* items, list_item* selected) {
    action_delete_one(&action_delete_system_save_data_def, items, selected);
}
