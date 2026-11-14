#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "dlqueue.h"
#include "resources.h"
#include "storescript.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/xferturn.h"

#define DLQUEUE_MAX 32
#define DLQUEUE_FREE_MS 4000      // A turn free this long after the transfer the queue paused for: it goes on.
#define DLQUEUE_ASK_AGAIN_MS 60000
#define DLQUEUE_FINISHED_MS 6000  // How long the toast says the queue has finished.

typedef enum {
    DLQ_QUEUED,
    DLQ_RUNNING,
    DLQ_PAUSED,
    DLQ_DONE,
    DLQ_FAILED
} dlq_state;

typedef struct {
    u32 id;
    char name[96];
    char* urls;
    void* script;        // A Store script's run (storescript.c) instead of URLs; NULL once it has ended.
    bool isScript;
    bool archives;
    void* userData;
    void (*finishedURL)(void* data, u32 index);
    void (*finishedAll)(void* data);
    bool released;       // finishedAll has run.
    dlq_state state;
    u32 ok;
    u32 failed;
    char error[160];
} dlq_item;

static dlq_item* dlq_items[DLQUEUE_MAX];
static u32 dlq_count;
static u32 dlq_next_id = 1;
static u32 dlq_version = 1;        // Changes with the list: the screen lists it again.

static void* dlq_run;              // The running item's download (installurl.c), or NULL.
static dlq_item* dlq_running;
static bool dlq_stop_queue;        // The running one stops because the whole queue pauses: it goes back to queued.
static bool dlq_paused;            // The whole queue.
static bool dlq_pause_after;       // Pause once the running one ends.
static bool dlq_for_transfer;      // Paused for an FTP or web transfer: goes on once the turn has been free a while.
static u64 dlq_free_since;
static u64 dlq_ask_after;
static bool dlq_asking;
static bool dlq_unseen;            // It finished with failures nobody has looked at.
static u32 dlq_session_ok;         // Since the queue last went idle.
static u32 dlq_session_failed;
static u64 dlq_finished_until;
static u32 dlq_finished_ok;

static ui_view* dlq_view;          // The queue screen, while open.

static void dlq_changed() {
    dlq_version++;
    ui_wake();
}

static dlq_item* dlq_find(u32 id) {
    for(u32 i = 0; i < dlq_count; i++) {
        if(dlq_items[i]->id == id) {
            return dlq_items[i];
        }
    }

    return NULL;
}

static s32 dlq_index(const dlq_item* item) {
    for(u32 i = 0; i < dlq_count; i++) {
        if(dlq_items[i] == item) {
            return (s32) i;
        }
    }

    return -1;
}

static void dlq_release(dlq_item* item) {
    if(!item->released) {
        item->released = true;
        if(item->finishedAll != NULL) {
            item->finishedAll(item->userData);
        }
    }
}

static void dlq_remove(dlq_item* item) {
    s32 index = dlq_index(item);
    if(index < 0) {
        return;
    }

    dlq_release(item);
    if(item->script != NULL && item != dlq_running) {
        storescript_queued_discard(item->script);
        item->script = NULL;
    }

    free(item->urls);
    free(item);
    memmove(&dlq_items[index], &dlq_items[index + 1], (dlq_count - (u32) index - 1) * sizeof(dlq_item*));
    dlq_count--;
    dlq_changed();
}

static u32 dlq_waiting() {
    u32 count = 0;
    for(u32 i = 0; i < dlq_count; i++) {
        if(dlq_items[i]->state == DLQ_QUEUED || dlq_items[i]->state == DLQ_PAUSED) {
            count++;
        }
    }

    return count;
}

bool dlqueue_add(const char* name, const char* urls, bool archives, void* userData, void (*finishedURL)(void* data, u32 index),
                 void (*finishedAll)(void* data)) {
    if(dlq_count >= DLQUEUE_MAX) {
        error_display(NULL, NULL, _("The download queue is full."));
        return false;
    }

    dlq_item* item = (dlq_item*) calloc(1, sizeof(dlq_item));
    char* copy = strdup(urls);
    if(item == NULL || copy == NULL) {
        free(item);
        free(copy);
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("The download queue is full."));
        return false;
    }

    item->id = dlq_next_id++;
    string_copy(item->name, name, sizeof(item->name));
    item->urls = copy;
    item->archives = archives;
    item->userData = userData;
    item->finishedURL = finishedURL;
    item->finishedAll = finishedAll;
    item->state = DLQ_QUEUED;
    dlq_items[dlq_count++] = item;
    dlq_finished_until = 0;
    dlq_changed();
    return true;
}

bool dlqueue_add_script(const char* name, void* script) {
    if(dlq_count >= DLQUEUE_MAX) {
        error_display(NULL, NULL, _("The download queue is full."));
        return false;
    }

    dlq_item* item = (dlq_item*) calloc(1, sizeof(dlq_item));
    if(item == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("The download queue is full."));
        return false;
    }

    item->id = dlq_next_id++;
    string_copy(item->name, name, sizeof(item->name));
    item->script = script;
    item->isScript = true;
    item->released = true;   // The script runs the Store's own end itself.
    item->state = DLQ_QUEUED;
    dlq_items[dlq_count++] = item;
    dlq_finished_until = 0;
    dlq_changed();
    return true;
}

// The running item, whatever it is (URLs or a script).
static void dlq_poll_running() {
    if(dlq_running != NULL && dlq_running->isScript) {
        storescript_queued_poll(dlq_run);
    } else {
        action_install_url_queued_poll(dlq_run);
    }
}

static void dlq_stop(bool pause) {
    if(dlq_running != NULL && dlq_running->isScript) {
        storescript_queued_stop(dlq_run, pause);
    } else {
        action_install_url_queued_stop(dlq_run, pause);
    }
}

static bool dlq_on_screen() {
    return dlq_running != NULL && dlq_running->isScript ? storescript_queued_on_screen(dlq_run) : action_install_url_queued_on_screen(dlq_run);
}

static void dlq_progress(u64* done, u64* total, char* name, size_t size) {
    if(dlq_running != NULL && dlq_running->isScript) {
        storescript_queued_progress(dlq_run, done, total, name, size);
    } else {
        action_install_url_queued_progress(dlq_run, done, total, name, size);
    }
}

// The running item's download has ended (installurl.c calls this before it lets go of it).
static void dlq_done(void* data, u32 ok, u32 failed, bool paused, bool cancelled, const char* error) {
    dlq_item* item = (dlq_item*) data;
    dlq_run = NULL;
    dlq_running = NULL;
    if(!paused) {
        item->script = NULL;   // A script's run is freed once it has ended.
    }

    if(cancelled) {
        dlq_remove(item);
    } else if(paused) {
        item->state = dlq_stop_queue ? DLQ_QUEUED : DLQ_PAUSED;
    } else {
        item->ok += ok;
        item->failed += failed;
        if(failed > 0) {
            item->state = DLQ_FAILED;
            string_copy(item->error, error != NULL && error[0] != '\0' ? error : _("Failed"), sizeof(item->error));
            dlq_session_failed++;
        } else {
            item->state = DLQ_DONE;
            dlq_release(item);
            dlq_session_ok++;
        }
    }

    dlq_stop_queue = false;
    if(dlq_pause_after && !paused) {
        dlq_paused = true;
        dlq_pause_after = false;
    }

    dlq_changed();
}

static void dlq_start(dlq_item* item) {
    item->state = DLQ_RUNNING;
    item->error[0] = '\0';
    dlq_running = item;
    if(item->isScript) {
        // It may end at once (and dlq_done clears dlq_run).
        dlq_run = item->script;
        storescript_queued_start(item->script, dlq_done, item);
        dlq_changed();
        return;
    }

    dlq_run = action_install_url_run_queued(item->urls, item->archives, item->userData, item->finishedURL, dlq_done, item);
    if(dlq_run == NULL) {
        dlq_running = NULL;
        item->state = DLQ_FAILED;
        string_copy(item->error, _("The download did not start."), sizeof(item->error));
    }

    dlq_changed();
}

// Stops the running one: paused (it waits), or because the whole queue pauses (it comes first again).
static void dlq_stop_running(bool wholeQueue) {
    if(dlq_run != NULL) {
        dlq_stop_queue = wholeQueue;
        dlq_stop(true);
    }
}

static void dlq_transfer_answered(ui_view* view, void* data, u32 response) {
    dlq_asking = false;
    if(response == PROMPT_YES) {
        dlq_for_transfer = true;
        dlq_free_since = 0;
        dlq_stop_running(true);
    } else {
        dlq_ask_after = osGetTime() + DLQUEUE_ASK_AGAIN_MS;
    }

    dlq_changed();
}

void dlqueue_tick() {
    if(dlq_run != NULL) {
        dlq_poll_running();
    }

    // An FTP or web transfer found the queue using the connection: the user decides.
    xfer_kind kind = XFER_FTP;
    char name[96];
    if(xferturn_wanted(&kind, name, sizeof(name)) && dlq_run != NULL && !dlq_asking && osGetTime() >= dlq_ask_after) {
        dlq_asking = true;
        const char* base = strrchr(name, '/');
        base = base != NULL && base[1] != '\0' ? base + 1 : name;
        static char text[256];
        snprintf(text, sizeof(text), kind == XFER_WEB ? _("The web page wants to send or receive %s, but the download queue is using the connection.\n\nPause the queue until that transfer ends?")
                                                      : _("An FTP program wants to send or receive %s, but the download queue is using the connection.\n\nPause the queue until that transfer ends?"),
                 base);
        prompt_display_yes_no(_("Download queue"), text, COLOR_TEXT, NULL, NULL, dlq_transfer_answered);
    }

    // Paused for a transfer: on again once the turn has been free a few seconds.
    if(dlq_for_transfer && dlq_run == NULL) {
        xferturn_status status;
        xferturn_get(&status);
        u64 now = osGetTime();
        if(status.busy) {
            dlq_free_since = 0;
        } else if(dlq_free_since == 0) {
            dlq_free_since = now;
        } else if(now - dlq_free_since >= DLQUEUE_FREE_MS) {
            dlq_for_transfer = false;
            dlq_changed();
        }
    }

    if(dlq_run != NULL || dlq_paused || dlq_for_transfer) {
        return;
    }

    for(u32 i = 0; i < dlq_count; i++) {
        if(dlq_items[i]->state == DLQ_QUEUED) {
            xferturn_status status;
            xferturn_get(&status);
            if(!status.busy) {
                dlq_start(dlq_items[i]);
            }

            return;
        }
    }

    // Nothing left to do: how it went.
    if(dlq_session_ok + dlq_session_failed > 0) {
        led_blink(dlq_session_failed == 0);
        if(dlq_session_failed > 0) {
            dlq_unseen = true;
        } else {
            dlq_finished_ok = dlq_session_ok;
            dlq_finished_until = osGetTime() + DLQUEUE_FINISHED_MS;
        }

        dlq_session_ok = 0;
        dlq_session_failed = 0;
        dlq_changed();
    }
}

bool dlqueue_notice(char* out, size_t size, float* progress) {
    // The queue screen open (or something it opened, as an item's actions) says it all already.
    if(dlq_view != NULL) {
        return false;
    }

    u32 waiting = dlq_waiting();
    if(dlq_run != NULL) {
        if(dlq_on_screen()) {
            return false;
        }

        u64 done = 0;
        u64 total = 0;
        char name[FILE_NAME_MAX];
        dlq_progress(&done, &total, name, sizeof(name));
        char head[128];
        u32 percent = total > 0 ? (u32) (done * 100 / total) : 0;
        if(waiting > 0) {
            snprintf(head, sizeof(head), _("Download queue, %lu %% (%lu more): tap to open"), percent, waiting);
        } else {
            snprintf(head, sizeof(head), _("Download queue, %lu %%: tap to open"), percent);
        }

        snprintf(out, size, "%s\n%s", head, name);
        *progress = total > 0 ? (float) ((double) done / (double) total) : WIDGET_PROGRESS_UNKNOWN;
        return true;
    }

    if((dlq_paused || dlq_for_transfer) && waiting > 0) {
        snprintf(out, size, dlq_for_transfer ? _("Download queue paused for a transfer (%lu waiting): tap to open") : _("Download queue paused (%lu waiting): tap to open"), waiting);
        *progress = -1;
        return true;
    }

    if(dlq_unseen) {
        u32 failed = 0;
        for(u32 i = 0; i < dlq_count; i++) {
            failed += dlq_items[i]->state == DLQ_FAILED;
        }

        if(failed > 0) {
            snprintf(out, size, _("Download queue: %lu failed. Tap to open"), failed);
            *progress = -1;
            return true;
        }

        dlq_unseen = false;
    }

    if(dlq_finished_until > osGetTime()) {
        snprintf(out, size, dlq_finished_ok == 1 ? _("Download queue finished: %lu done") : _("Download queue finished: %lu done"), dlq_finished_ok);
        *progress = -1;
        return true;
    }

    return false;
}

bool dlqueue_open_any() {
    if(dlq_count == 0 || (dlq_view != NULL && ui_top() == dlq_view)) {
        return false;
    }

    dlqueue_open();
    return true;
}

// ---- the queue screen ----

static u32 dlq_seen_version;
static u32 dlq_sheet_id;   // The item whose actions are open.

static const char* dlq_state_text(const dlq_item* item) {
    switch(item->state) {
        case DLQ_QUEUED: return _("Queued");
        case DLQ_RUNNING: return _("Running");
        case DLQ_PAUSED: return _("Paused");
        case DLQ_DONE: return _("Done");
        case DLQ_FAILED: return _("Failed");
    }

    return "";
}

static void dlq_clear_items(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void dlq_populate(ui_view* view, linked_list* items, list_item* selected) {
    u32 keep = selected != NULL ? (u32) (uintptr_t) selected->data : 0;
    dlq_clear_items(items);
    list_item* select = NULL;
    for(u32 i = 0; i < dlq_count; i++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            break;
        }

        string_copy(item->name, dlq_items[i]->name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = (void*) (uintptr_t) dlq_items[i]->id;
        linked_list_add(items, item);
        if(dlq_items[i]->id == keep) {
            select = item;
        }
    }

    if(select != NULL) {
        list_select(view, select);
    }
}

static void dlq_draw_row(void* data, list_item* row, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);
    dlq_item* item = dlq_find((u32) (uintptr_t) row->data);
    if(item == NULL) {
        return;
    }

    int icon = ICON_CLOCK;
    u32 color = muted;
    char status[48];
    string_copy(status, dlq_state_text(item), sizeof(status));
    if(item->state == DLQ_RUNNING) {
        icon = ICON_DOWNLOAD;
        color = selected ? ink : t->accent;
        if(dlq_run != NULL) {
            u64 done = 0;
            u64 total = 0;
            char name[FILE_NAME_MAX];
            dlq_progress(&done, &total, name, sizeof(name));
            if(total > 0) {
                snprintf(status, sizeof(status), "%lu %%", (u32) (done * 100 / total));
            }
        }
    } else if(item->state == DLQ_DONE) {
        icon = ICON_CHECK;
        color = selected ? ink : t->palette->success;
    } else if(item->state == DLQ_FAILED) {
        icon = ICON_X;
        color = selected ? ink : t->palette->danger;
    } else if(item->state == DLQ_PAUSED) {
        icon = ICON_PLAYER_PAUSE;
        color = selected ? ink : t->accent;
    }

    screen_draw_icon(icon, x + 8, y + (height - 16) / 2, 16, color);
    float statusW = widget_text_width(status, WIDGET_SMALL_SCALE);
    screen_draw_text(status, x + width - 10 - statusW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, color);
    float tx = x + 32;
    widget_text_fit(item->name, tx, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink, x + width - 18 - statusW - tx);
}

static void dlq_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    const theme_current* t = theme();
    float cardX = x1 + 12;
    float cardY = y1 + 10;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 20;
    widget_card(cardX, cardY, cardW, cardH);
    float x = cardX + 14;
    float width = cardW - 28;
    float y = cardY + 12;
    float line = widget_text_height(WIDGET_SMALL_SCALE) + 4;

    // The queue: running, paused, or waiting to pause.
    char head[128];
    u32 waiting = dlq_waiting();
    if(dlq_for_transfer) {
        snprintf(head, sizeof(head), _("Paused for a transfer: it goes on when that ends (%lu waiting)."), waiting);
    } else if(dlq_paused) {
        snprintf(head, sizeof(head), _("Paused (%lu waiting)."), waiting);
    } else if(dlq_pause_after) {
        string_copy(head, _("Pauses when the current one ends."), sizeof(head));
    } else if(dlq_run != NULL) {
        snprintf(head, sizeof(head), _("Running (%lu more waiting)."), waiting);
    } else {
        string_copy(head, waiting > 0 ? _("Waiting for the connection.") : _("Nothing waiting."), sizeof(head));
    }

    widget_text_fit(_("Download queue"), x, y, WIDGET_TITLE_SCALE, t->palette->text, width);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 6;
    screen_draw_text_wrap(head, x, y, WIDGET_SMALL_SCALE, t->accent, x + width);
    y += line * 2 + 4;

    dlq_item* item = selected != NULL ? dlq_find((u32) (uintptr_t) selected->data) : NULL;
    if(item == NULL) {
        return;
    }

    widget_text_fit(item->name, x, y, WIDGET_TEXT_SCALE, t->palette->text, width);
    y += widget_text_height(WIDGET_TEXT_SCALE) + 4;
    widget_text_fit(dlq_state_text(item), x, y, WIDGET_SMALL_SCALE, t->palette->textMuted, width);
    y += line;

    if(item->state == DLQ_RUNNING && dlq_run != NULL) {
        u64 done = 0;
        u64 total = 0;
        char name[FILE_NAME_MAX];
        dlq_progress(&done, &total, name, sizeof(name));
        char figures[64];
        ui_format_size(figures, sizeof(figures), done);
        if(total > 0) {
            size_t used = strlen(figures);
            snprintf(figures + used, sizeof(figures) - used, " / ");
            used = strlen(figures);
            ui_format_size(figures + used, sizeof(figures) - used, total);
        }

        widget_text_fit(name, x, y, WIDGET_SMALL_SCALE, t->palette->text, width);
        y += line;
        widget_text_fit(figures, x, y, WIDGET_SMALL_SCALE, t->palette->textMuted, width);
        y += line + 2;
        float progress = total > 0 ? (float) ((double) done / (double) total) : 0;
        screen_fill_round_rect(x, y, width, 6, 3, t->accentSoft);
        screen_fill_round_rect(x, y, width * (progress > 1 ? 1 : progress), 6, 3, t->accent);
    } else if(item->state == DLQ_FAILED && item->error[0] != '\0') {
        screen_draw_text_wrap(item->error, x, y, WIDGET_SMALL_SCALE, t->palette->danger, x + width);
    }
}

// The actions of an item: what its state allows.
static void dlq_act_open(void* data) {
    if(dlq_run != NULL && dlq_running != NULL && dlq_running->id == dlq_sheet_id) {
        if(dlq_running->isScript) {
            storescript_queued_open(dlq_run);
        } else {
            action_install_url_queued_open(dlq_run);
        }
    }
}

static void dlq_act_pause(void* data) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    if(item == NULL) {
        return;
    }

    if(item->state == DLQ_RUNNING) {
        dlq_stop_running(false);
    } else if(item->state == DLQ_QUEUED) {
        item->state = DLQ_PAUSED;
    }

    dlq_changed();
}

static void dlq_act_resume(void* data) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    if(item != NULL && (item->state == DLQ_PAUSED || item->state == DLQ_FAILED)) {
        item->state = DLQ_QUEUED;
        item->error[0] = '\0';
        dlq_changed();
    }
}

static void dlq_move(s32 to) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    s32 from = item != NULL ? dlq_index(item) : -1;
    if(from < 0 || to < 0 || to >= (s32) dlq_count || to == from) {
        return;
    }

    memmove(from < to ? &dlq_items[from] : &dlq_items[to + 1], from < to ? &dlq_items[from + 1] : &dlq_items[to],
            (size_t) (from < to ? to - from : from - to) * sizeof(dlq_item*));
    dlq_items[to] = item;
    dlq_changed();
}

static void dlq_act_first(void* data) { dlq_move(0); }
static void dlq_act_up(void* data) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    if(item != NULL) {
        dlq_move(dlq_index(item) - 1);
    }
}
static void dlq_act_down(void* data) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    if(item != NULL) {
        dlq_move(dlq_index(item) + 1);
    }
}

static void dlq_act_cancel(void* data) {
    dlq_item* item = dlq_find(dlq_sheet_id);
    if(item == NULL) {
        return;
    }

    if(item->state == DLQ_RUNNING && dlq_run != NULL) {
        dlq_stop(false);
    } else {
        dlq_remove(item);
    }
}

static void dlq_show_actions(dlq_item* item) {
    dlq_sheet_id = item->id;
    sheet_action actions[8];
    u32 count = 0;
    switch(item->state) {
        case DLQ_RUNNING:
            actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Show its progress"), false, dlq_act_open, false, NULL};
            actions[count++] = (sheet_action) {ICON_PLAYER_PAUSE, _("Pause this one"), false, dlq_act_pause, false, NULL};
            actions[count++] = (sheet_action) {ICON_X, _("Cancel"), true, dlq_act_cancel, false, NULL};
            break;
        case DLQ_QUEUED:
        case DLQ_PAUSED:
            if(item->state == DLQ_QUEUED) {
                actions[count++] = (sheet_action) {ICON_PLAYER_PAUSE, _("Pause this one"), false, dlq_act_pause, false, NULL};
            } else {
                actions[count++] = (sheet_action) {ICON_PLAYER_PLAY, _("Resume this one"), false, dlq_act_resume, false, NULL};
            }

            actions[count++] = (sheet_action) {ICON_ARROWS_SORT, _("Put first"), false, dlq_act_first, false, NULL};
            actions[count++] = (sheet_action) {ICON_CHEVRON_UP, _("Move up"), false, dlq_act_up, false, NULL};
            actions[count++] = (sheet_action) {ICON_CHEVRON_DOWN, _("Move down"), false, dlq_act_down, false, NULL};
            actions[count++] = (sheet_action) {ICON_X, _("Cancel"), true, dlq_act_cancel, false, NULL};
            break;
        case DLQ_FAILED:
            if(!item->isScript) {
                actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Retry"), false, dlq_act_resume, false, NULL};
            }

            actions[count++] = (sheet_action) {ICON_TRASH, _("Remove"), false, dlq_act_cancel, false, NULL};
            break;
        case DLQ_DONE:
            actions[count++] = (sheet_action) {ICON_TRASH, _("Remove"), false, dlq_act_cancel, false, NULL};
            break;
    }

    sheet_display_list(item->name, 0, actions, count, NULL);
}

static void dlq_clear_finished() {
    for(u32 i = dlq_count; i > 0; i--) {
        dlq_item* item = dlq_items[i - 1];
        if(item->state == DLQ_DONE || item->state == DLQ_FAILED) {
            dlq_remove(item);
        }
    }
}

static void dlq_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    u32 down = hidKeysDown();
    if(down & KEY_B) {
        ui_pop();
        dlq_clear_items(items);
        list_destroy(view);
        dlq_view = NULL;
        dlq_unseen = false;
        return;
    }

    if(dlq_seen_version != dlq_version) {
        dlq_seen_version = dlq_version;
        dlq_populate(view, items, selected);
        return;
    }

    bool stopped = dlq_paused || dlq_for_transfer;
    view->info = stopped ? _(" actions    resume queue    clear finished    back")
                         : dlq_pause_after ? _(" actions    pause now    do not pause    clear finished    back")
                                           : _(" actions    pause now    pause after this one    clear finished    back");

    if(down & KEY_X) {
        if(stopped) {
            dlq_paused = false;
            dlq_for_transfer = false;
            dlq_pause_after = false;
        } else {
            dlq_paused = true;
            dlq_pause_after = false;
            dlq_stop_running(true);
        }

        dlq_changed();
    } else if(down & KEY_Y) {
        if(!stopped && dlq_run != NULL) {
            dlq_pause_after = !dlq_pause_after;
            dlq_changed();
        }
    } else if(down & KEY_R) {
        dlq_clear_finished();
    } else if(selected != NULL && (selectedTouched || (down & KEY_A))) {
        dlq_item* item = dlq_find((u32) (uintptr_t) selected->data);
        if(item != NULL) {
            dlq_show_actions(item);
        }
    }
}

void dlqueue_open() {
    if(dlq_view != NULL) {
        return;
    }

    ui_view* view = list_display(_("Download queue"), _(" actions    pause now    back"), NULL, dlq_update, dlq_draw_top);
    if(view == NULL) {
        return;
    }

    static const list_style style = {38, dlq_draw_row, 0, NULL};
    list_set_style(view, &style);
    list_show_title_bar(view);
    dlq_view = view;
    dlq_seen_version = 0;
    dlq_unseen = false;
}
