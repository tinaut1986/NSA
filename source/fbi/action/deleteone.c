#include <malloc.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    const action_delete_one_def* def;
    linked_list* items;   // NULL: `selected` is a copy made for this action, which frees it.
    list_item* selected;
} delete_one_data;

static void action_delete_one_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    delete_one_data* deleteData = (delete_one_data*) data;
    deleteData->def->drawInfo(view, deleteData->selected->data, x1, y1, x2, y2);
}

static void action_delete_one_update(ui_view* view, void* data, float* progress, char* text) {
    delete_one_data* deleteData = (delete_one_data*) data;
    const action_delete_one_def* def = deleteData->def;
    void* info = deleteData->selected->data;

    Result res = def->run(deleteData->selected);

    ui_pop();
    info_destroy(view);

    if(R_FAILED(res)) {
        error_display_res(info, def->drawInfo, res, def->failure);
    } else {
        if(deleteData->items != NULL) {
            linked_list_remove(deleteData->items, deleteData->selected);
        }

        def->freeItem(deleteData->selected);

        prompt_display_notify("Success", def->success, COLOR_TEXT, NULL, NULL, NULL);
    }

    free(data);
}

static void action_delete_one_onresponse(ui_view* view, void* data, u32 response) {
    delete_one_data* deleteData = (delete_one_data*) data;
    if(response == PROMPT_YES) {
        info_display(deleteData->def->progress, "", false, data, action_delete_one_update, action_delete_one_draw_top);
    } else {
        if(deleteData->items == NULL) {
            deleteData->def->freeItem(deleteData->selected);
        }

        free(data);
    }
}

void action_delete_one(const action_delete_one_def* def, linked_list* items, list_item* selected) {
    delete_one_data* data = (delete_one_data*) calloc(1, sizeof(delete_one_data));
    if(data == NULL) {
        error_display(NULL, NULL, def->allocError);
        return;
    }

    data->def = def;
    data->items = items;
    data->selected = selected;

    prompt_display_warning("Confirmation", def->question, COLOR_TEXT, data, action_delete_one_draw_top, action_delete_one_onresponse);
}
