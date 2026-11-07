#include <malloc.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    linked_list* items;
    list_item* selected;
    bool ticket;
    bool mods;   // Delete the game's mods in /luma/titles too, once the title is gone.
} delete_title_data;

static void action_delete_title_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((delete_title_data*) data)->selected->data, x1, y1, x2, y2);
}

static void action_delete_title_update(ui_view* view, void* data, float* progress, char* text) {
    delete_title_data* deleteData = (delete_title_data*) data;

    title_info* info = (title_info*) deleteData->selected->data;

    Result res = 0;

    if(R_SUCCEEDED(res = AM_DeleteTitle(info->mediaType, info->titleId)) && deleteData->ticket) {
        res = AM_DeleteTicket(info->titleId);
    }

    ui_pop();
    info_destroy(view);

    if(R_FAILED(res)) {
        error_display_res(info, task_draw_title_info, res, "Failed to delete title.");
    } else {
        u64 titleId = info->titleId;
        linked_list_remove(deleteData->items, deleteData->selected);
        task_free_title(deleteData->selected);

        if(!deleteData->mods) {
            prompt_display_notify("Success", "Title deleted.", COLOR_TEXT, NULL, NULL, NULL);
        } else if(R_FAILED(res = task_mods_delete(titleId))) {
            error_display_res(NULL, NULL, res, "Failed to delete the mods.");
        } else {
            prompt_display_notify("Success", "Title and mods deleted.", COLOR_TEXT, NULL, NULL, NULL);
        }
    }

    free(data);
}

static void action_delete_title_start(delete_title_data* data) {
    info_display("Deleting Title", "", false, data, action_delete_title_update, action_delete_title_draw_top);
}

// Mods are of no use without the game, but the user may keep them for a reinstall or for the game card. Asked before
// anything is deleted: if the title cannot be deleted, its mods stay.
static void action_delete_title_mods_answer(ui_view* view, void* data, u32 response) {
    delete_title_data* deleteData = (delete_title_data*) data;
    deleteData->mods = response == PROMPT_YES;
    action_delete_title_start(deleteData);
}

static void action_delete_title_onresponse(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        free(data);
        return;
    }

    delete_title_data* deleteData = (delete_title_data*) data;
    u64 titleId = ((title_info*) deleteData->selected->data)->titleId;
    title_kind kind = title_kind_of(titleId);
    if((kind == TITLE_KIND_GAME || kind == TITLE_KIND_DEMO) && task_mods_exist(titleId)) {
        prompt_display_warning("Game mods", "This game has mods in /luma/titles (LayeredFS). Delete them too?", COLOR_TEXT, data, action_delete_title_draw_top, action_delete_title_mods_answer);
    } else {
        action_delete_title_start(deleteData);
    }
}

static void action_delete_title_internal(linked_list* items, list_item* selected, const char* message, bool ticket) {
    delete_title_data* data = (delete_title_data*) calloc(1, sizeof(delete_title_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate delete title data.");

        return;
    }

    data->items = items;
    data->selected = selected;
    data->ticket = ticket;

    prompt_display_warning("Confirmation", message, COLOR_TEXT, data, action_delete_title_draw_top, action_delete_title_onresponse);
}

void action_delete_title(linked_list* items, list_item* selected) {
    action_delete_title_internal(items, selected, "Delete the selected title?", false);
}

void action_delete_title_ticket(linked_list* items, list_item* selected) {
    action_delete_title_internal(items, selected, "Delete the selected title and ticket?", true);
}