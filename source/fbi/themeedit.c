// The theme editor: a list of the theme's values (list.c, Left and Right change the selected one, quicker while held),
// its colours picked in colorpick.c. While it is open everything is drawn with the copy being edited (theme_preview), in
// the palette whose colours show (day or night). It starts from the theme in use, and "Start from" takes any other or a
// blank template; a theme from the SD card can be saved over its file, saved as a new one, or deleted.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "../core/core.h"
#include "../core/ui/keys.h"
#include "colorpick.h"
#include "settings.h"
#include "themeedit.h"

#define TE_BLANK UINT32_MAX   // "Start from" the blank template (theme_blank) instead of a theme of the list.

typedef enum {
    TE_FROM,
    TE_NAME,
    TE_PALETTE,
    TE_COLOR_FIRST,
    TE_COLOR_LAST = TE_COLOR_FIRST + 9,
    TE_RADIUS,
    TE_BORDER,
    TE_FONT,
    TE_BARS,
    TE_DIALOGS,
    TE_MENU,
    TE_GRID,
    TE_ACCENT,
    TE_SAVE,        // Over the file (a theme from the SD card only).
    TE_SAVE_NEW,
    TE_DELETE,      // A theme from the SD card only.
    TE_COUNT
} te_row;

typedef struct {
    const char* label;   // English.
    size_t offset;       // In theme_palette.
    bool alpha;          // May be see-through.
} te_color;

static const te_color te_colors[10] = {
        {"Background", offsetof(theme_palette, background), false},
        {"Surface", offsetof(theme_palette, surface), false},
        {"Border", offsetof(theme_palette, border), false},
        {"Text", offsetof(theme_palette, text), false},
        {"Secondary text", offsetof(theme_palette, textMuted), false},
        {"Behind dialogs", offsetof(theme_palette, scrim), true},
        {"Success", offsetof(theme_palette, success), false},
        {"Success badge", offsetof(theme_palette, successSoft), false},
        {"Danger", offsetof(theme_palette, danger), false},
        {"Warning", offsetof(theme_palette, warning), false}
};

typedef struct {
    ui_view* view;
    theme_def def;
    char name[48];
    char id[8];
    u32 editing;          // Where it started from: a theme's index, or TE_BLANK.
    char fromName[48];    // Its name, as it shows.
    bool night;           // Which palette the colour rows show and the screens are drawn in.
    bool dirty;
    bool closing;
    u32 pickingRow;
    u32 pendingFrom;      // A "Start from" waiting for the answer to "discard the changes?".
    u64 holdAt[2];        // Left and Right held.
} themeedit_data;

static bool te_custom(themeedit_data* d) {
    return d->editing != TE_BLANK && theme_is_custom(d->editing);
}

static theme_palette* te_palette(themeedit_data* d) {
    return d->night ? &d->def.night : &d->def.day;
}

static u32* te_color_of(themeedit_data* d, u32 row) {
    return (u32*) ((char*) te_palette(d) + te_colors[row - TE_COLOR_FIRST].offset);
}

static void te_preview(themeedit_data* d) {
    d->def.name = d->name;
    d->def.id = d->id;
    theme_preview(&d->def, d->night ? 1 : 0);
}

// Starts again from the theme at `index` (or the blank template): a built-in one is saved as a new theme named
// "<Name> 2", one from the SD card keeps its name.
static void te_load(themeedit_data* d, u32 index) {
    d->editing = index < theme_count() ? index : TE_BLANK;
    if(d->editing == TE_BLANK) {
        d->def = *theme_blank();
        string_copy(d->fromName, _("Blank template"), sizeof(d->fromName));
        string_copy(d->name, _("My theme"), sizeof(d->name));
    } else {
        d->def = *theme_get(d->editing);
        string_copy(d->fromName, i18n_translate_text(d->def.name), sizeof(d->fromName));
        if(theme_is_custom(d->editing)) {
            string_copy(d->name, d->def.name, sizeof(d->name));
        } else {
            snprintf(d->name, sizeof(d->name), "%s 2", d->def.name);
        }
    }

    d->dirty = false;
    te_preview(d);
}

static const char* te_label(u32 row) {
    switch(row) {
        case TE_FROM: return "Start from";
        case TE_NAME: return "Name";
        case TE_PALETTE: return "Colours of";
        case TE_RADIUS: return "Corners";
        case TE_BORDER: return "Outline width";
        case TE_FONT: return "Font";
        case TE_BARS: return "Progress bars";
        case TE_DIALOGS: return "Dialogs";
        case TE_MENU: return "Main menu";
        case TE_GRID: return "Icon grids";
        case TE_ACCENT: return "Accent colour";
        case TE_SAVE: return "Save the theme";
        case TE_SAVE_NEW: return "Save as a new theme";
        case TE_DELETE: return "Delete this theme";
        default: return te_colors[row - TE_COLOR_FIRST].label;
    }
}

// The row's value, translated; NULL for the rows that only run something.
static const char* te_value(themeedit_data* d, u32 row, char* buf, size_t size) {
    switch(row) {
        case TE_FROM: return d->fromName;
        case TE_NAME: return d->name;
        case TE_PALETTE: return d->night ? _("Night") : _("Day");
        case TE_RADIUS: snprintf(buf, size, "%d", (int) d->def.radius); return buf;
        case TE_BORDER: snprintf(buf, size, "%d", (int) d->def.border); return buf;
        case TE_FONT: return d->def.pixelFont ? _("Pixel") : _("System");
        case TE_BARS: return d->def.blockBars ? _("Blocks") : _("Smooth");
        case TE_DIALOGS: return d->def.sheetPrompts ? _("Sheet from below") : _("Window");
        case TE_MENU: return d->def.menuLayout == THEME_MENU_TILES ? _("Big tiles") : _("Buttons");
        case TE_GRID: return d->def.pagedGrids ? _("Pages sideways") : _("Scroll down");
        case TE_ACCENT: return i18n_translate_text(theme_accent_get(settings_get()->accent)->name);
        default: return NULL;
    }
}

// The saving rows show only when they can be used: over the file and deleting, only for a theme of the SD card.
static bool te_visible(void* data, list_item* item) {
    themeedit_data* d = (themeedit_data*) data;
    u32 row = (u32) (uintptr_t) item->data;
    return (row != TE_SAVE && row != TE_DELETE) || te_custom(d);
}

// A value at the right of the row, without the arrows of one that steps.
static void te_draw_plain_value(const char* value, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    float right = widget_row_chevron(x + width - 8, y, height, selected);
    float tw = widget_text_width(value, WIDGET_SMALL_SCALE);
    float vx = right - tw;
    screen_draw_text(value, vx > x + width / 2 ? vx : x + width / 2, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2,
                     WIDGET_SMALL_SCALE, selected ? t->onAccent : t->palette->textMuted);
}

static void te_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    themeedit_data* d = (themeedit_data*) data;
    const theme_current* t = theme();
    u32 row = (u32) (uintptr_t) item->data;
    char buf[16];

    if(row >= TE_COLOR_FIRST && row <= TE_COLOR_LAST) {
        // The colour as a swatch at the right, with its code.
        widget_row(x, y, width, height, ICON_COLOR_SWATCH, i18n_translate_text(te_label(row)), t->palette->text, NULL, selected);
        u32 c = *te_color_of(d, row);
        float sw = 30;
        float sx = x + width - 10 - sw;
        float sy = y + 5;
        screen_fill_round_rect(sx - 1, sy - 1, sw + 2, height - 8, 4, selected ? t->onAccent : t->palette->border);
        screen_fill_round_rect(sx, sy, sw, height - 10, 3, c | 0xFF000000u);
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", (unsigned) (c & 0xFF), (unsigned) ((c >> 8) & 0xFF), (unsigned) ((c >> 16) & 0xFF));
        float tw = widget_text_width(buf, WIDGET_SMALL_SCALE);
        screen_draw_text(buf, sx - 6 - tw, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE,
                         selected ? t->onAccent : t->palette->textMuted);
        return;
    }

    static const int icons[TE_COUNT] = {
            [TE_FROM] = ICON_COPY, [TE_NAME] = ICON_PENCIL, [TE_PALETTE] = ICON_SUN_MOON, [TE_RADIUS] = ICON_SQUARE,
            [TE_BORDER] = ICON_SQUARE, [TE_FONT] = ICON_ABC, [TE_BARS] = ICON_HOURGLASS, [TE_DIALOGS] = ICON_APP_WINDOW,
            [TE_MENU] = ICON_LAYOUT_GRID, [TE_GRID] = ICON_LAYOUT_GRID, [TE_ACCENT] = ICON_COLOR_SWATCH, [TE_SAVE] = ICON_DEVICE_FLOPPY,
            [TE_SAVE_NEW] = ICON_PLUS, [TE_DELETE] = ICON_TRASH
    };
    const char* label = i18n_translate_text(te_label(row));
    const char* value = te_value(d, row, buf, sizeof(buf));
    if(row == TE_FROM || row == TE_NAME) {
        // Chosen from a list or typed, not stepped through: no arrows.
        widget_row(x, y, width, height, icons[row], label, t->palette->text, NULL, selected);
        te_draw_plain_value(value, x, y, width, height, selected);
        return;
    }

    u32 color = t->palette->text;
    if(!selected && (row == TE_SAVE || row == TE_SAVE_NEW)) {
        color = t->accent;
    } else if(!selected && row == TE_DELETE) {
        color = t->palette->danger;
    }

    widget_row(x, y, width, height, icons[row], label, color, value, selected);
}

// A small picture of the theme on the top screen: a card, rows, a button, a bar and the colours of success and danger.
static void te_draw_preview(themeedit_data* d, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();
    float x = x1 + 12;
    float w = x2 - x1 - 24;
    float y = y1 + 10;

    widget_card(x, y, w, 70);
    widget_text_fit(d->name, x + 14, y + 10, WIDGET_TITLE_SCALE, t->palette->text, w - 28);
    widget_text_fit(_("A sample of how this theme looks."), x + 14, y + 36, WIDGET_TEXT_SCALE, t->palette->textMuted, w - 28);
    float bw = widget_badge_right(x + w - 12, y + 52, _("In use"), t->palette->successSoft, t->palette->success);
    widget_text_fit(_("Delete"), x + 14, y + 52, WIDGET_SMALL_SCALE, t->palette->danger, w - bw - 40);
    y += 80;

    widget_row(x, y, w, WIDGET_ROW_HEIGHT, ICON_FOLDER, _("Files"), t->palette->text, NULL, false);
    y += WIDGET_ROW_HEIGHT + 6;
    widget_row(x, y, w, WIDGET_ROW_HEIGHT, ICON_APPS, _("Titles"), t->palette->text, NULL, true);
    y += WIDGET_ROW_HEIGHT + 10;

    widget_button(x, y, 110, 28, ICON_CHECK, _("Button"), true);
    widget_progress(x + 122, y + 10, w - 122 - 70, 8, 0.64f);

    // A dialog over the veil behind it.
    float dx = x + w - 60;
    screen_fill_round_rect(dx, y - 4, 60, 36, t->def->radiusSmall, t->palette->scrim);
    screen_frame_round_rect(dx + 10, y + 4, 40, 20, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
}

static void te_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    te_draw_preview((themeedit_data*) data, x1, y1, x2, y2);
}

static void te_picker_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    te_draw_preview((themeedit_data*) data, x1, y1, x2, y2);
}

static void te_picker_changed(void* data, u32 color) {
    themeedit_data* d = (themeedit_data*) data;
    *te_color_of(d, d->pickingRow) = color;
    te_preview(d);
}

static void te_picker_done(void* data, bool accepted, u32 color) {
    themeedit_data* d = (themeedit_data*) data;
    *te_color_of(d, d->pickingRow) = color;
    d->dirty = d->dirty || accepted;
    te_preview(d);
}

static void te_name_typed(ui_view* kbd, void* data, SwkbdButton button, const char* response) {
    themeedit_data* d = (themeedit_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM && response != NULL && response[0] != '\0') {
        string_copy(d->name, response, sizeof(d->name));
        d->dirty = true;
        te_preview(d);
    }
}

static void te_free_items(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void te_close(themeedit_data* d) {
    ui_view* view = d->view;
    theme_preview_end();
    ui_pop();
    te_free_items(list_get_items(view));
    list_destroy(view);
    free(d);
}

// Saves over the file (`asNew` false, a theme of the SD card) or as a new one, and puts it in use.
static bool te_save(themeedit_data* d, bool asNew) {
    settings_data* settings = settings_get();
    char path[256];
    u32 saved = 0;
    // A built-in theme's index makes a new file.
    u32 over = !asNew && te_custom(d) ? d->editing : 0;
    if(!theme_save_custom(&d->def, d->name, over, &saved, path, sizeof(path))) {
        error_display(NULL, NULL, _("The theme could not be saved to the SD card."));
        return false;
    }

    settings->theme = saved;
    te_close(d);
    settings_apply();
    settings_save();

    char text[256];
    snprintf(text, sizeof(text), _("Saved as %s, and in use."), strncmp(path, "sdmc:", 5) == 0 ? path + 5 : path);
    prompt_display_notify(_("Theme saved"), text, COLOR_TEXT, NULL, NULL, NULL);
    return true;
}

static void te_leave_answer(ui_view* view, void* data, u32 response) {
    themeedit_data* d = (themeedit_data*) data;
    // 0 saves the changes (over the file of a theme of the SD card, else as a new one), 1 throws them away, 2 (B) keeps editing.
    if(response == 0) {
        te_save(d, !te_custom(d));
    } else if(response == 1) {
        te_close(d);
    } else {
        d->closing = false;
    }
}

static void te_delete_answer(ui_view* view, void* data, u32 response) {
    themeedit_data* d = (themeedit_data*) data;
    d->closing = false;
    if(response != PROMPT_YES) {
        return;
    }

    settings_data* settings = settings_get();
    bool inUse = settings->theme == d->editing;
    char id[64];
    string_copy(id, theme_get(settings->theme)->id, sizeof(id));
    if(!theme_delete_custom(d->editing)) {
        error_display(NULL, NULL, _("The theme could not be deleted from the SD card."));
        return;
    }

    // The theme in use stays, unless it was this one: then the first built-in one.
    settings->theme = inUse ? 0 : theme_find(id);
    te_close(d);
    settings_apply();
    settings_save();
}

// ---- "Start from": a list of the themes and the blank template ----

static void te_from_answer(ui_view* view, void* data, u32 response) {
    themeedit_data* d = (themeedit_data*) data;
    if(response == PROMPT_YES) {
        te_load(d, d->pendingFrom);
    }
}

static void te_from_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    themeedit_data* d = (themeedit_data*) data;
    view->name = _("Start from");
    view->info = _(" choose    back");

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i <= theme_count(); i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                // The themes, then the blank template.
                string_copy(item->name, i < theme_count() ? theme_get(i)->name : "Blank template", sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) (i < theme_count() ? i : TE_BLANK);
                linked_list_add(items, item);
                if(i == d->editing || (i == theme_count() && d->editing == TE_BLANK)) {
                    list_select(view, item);
                }
            }
        }
    }

    bool chosen = selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A));
    if(!chosen && !(hidKeysDown() & KEY_B)) {
        return;
    }

    u32 from = chosen ? (u32) (uintptr_t) selected->data : 0;
    ui_pop();
    te_free_items(items);
    list_destroy(view);
    if(!chosen) {
        return;
    }

    if(d->dirty) {
        d->pendingFrom = from;
        prompt_display_yes_no("Start from", _("Discard the changes and start from that theme?"), COLOR_TEXT, d, NULL, te_from_answer);
    } else {
        te_load(d, from);
    }
}

static void te_from_open(themeedit_data* d) {
    ui_view* view = list_display(_("Start from"), _(" choose    back"), d, te_from_update, te_draw_top);
    if(view != NULL) {
        list_show_title_bar(view);
    }
}

// ---- changing a value ----

// Left and Right stop at the ends; A (step 0) goes up and round.
static int te_step_number(float* value, int step, int max) {
    int v = (int) *value + (step != 0 ? step : 1);
    v = step == 0 && v > max ? 0 : v < 0 ? 0 : v > max ? max : v;
    *value = (float) v;
    return v;
}

// Left (-1), Right (1) or A (0) on a row.
static void te_change(themeedit_data* d, u32 row, int step) {
    bool turn = step != 0 || row >= TE_FONT;
    switch(row) {
        case TE_FROM:
            if(step == 0) {
                te_from_open(d);
            }
            return;
        case TE_NAME:
            if(step == 0) {
                osk_display(_("Name of the theme"), d->name, sizeof(d->name) - 1, 0, d, te_name_typed);
            }
            return;
        case TE_PALETTE:
            d->night = !d->night;
            te_preview(d);
            return;
        case TE_RADIUS:
            // One value: the corners of rows and buttons follow those of cards.
            te_step_number(&d->def.radius, step, 24);
            d->def.radiusSmall = theme_small_radius(d->def.radius);
            break;
        case TE_BORDER: te_step_number(&d->def.border, step, 4); break;
        case TE_FONT: if(turn) d->def.pixelFont = !d->def.pixelFont; break;
        case TE_BARS: if(turn) d->def.blockBars = !d->def.blockBars; break;
        case TE_DIALOGS: if(turn) d->def.sheetPrompts = !d->def.sheetPrompts; break;
        case TE_MENU: if(turn) d->def.menuLayout = d->def.menuLayout == THEME_MENU_TILES ? THEME_MENU_GRID2 : THEME_MENU_TILES; break;
        case TE_GRID: if(turn) d->def.pagedGrids = !d->def.pagedGrids; break;
        case TE_ACCENT: {
            // Not part of the theme: the settings' own, changed here to see it with the theme.
            settings_data* settings = settings_get();
            u32 count = theme_accent_count();
            settings->accent = (settings->accent + count + (step < 0 ? count - 1 : 1)) % count;
            settings_apply();
            settings_save();
            te_preview(d);
            return;
        }
        case TE_SAVE:
            if(step == 0) {
                te_save(d, false);
            }
            return;
        case TE_SAVE_NEW:
            if(step == 0) {
                te_save(d, true);
            }
            return;
        case TE_DELETE:
            if(step == 0) {
                char text[160];
                snprintf(text, sizeof(text), _("Delete the theme \"%s\"? Its file is removed from the SD card."), theme_get(d->editing)->name);
                d->closing = true;
                prompt_display_warning("Delete this theme", text, COLOR_TEXT, d, NULL, te_delete_answer);
            }
            return;
        default:
            if(step == 0) {
                d->pickingRow = row;
                const te_color* c = &te_colors[row - TE_COLOR_FIRST];
                colorpick_open(i18n_translate_text(c->label), *te_color_of(d, row), c->alpha, d, te_picker_draw_top, te_picker_changed, te_picker_done);
            }
            return;
    }

    d->dirty = true;
    te_preview(d);
}

static void te_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    themeedit_data* d = (themeedit_data*) data;
    view->name = _("Edit the theme");
    view->info = _(" change    back");

    if(linked_list_size(items) == 0) {
        for(u32 row = 0; row < TE_COUNT; row++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, te_label(row), sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) row;
                linked_list_add(items, item);
            }
        }
    }

    if(d->closing) {
        return;
    }

    if(hidKeysDown() & KEY_B) {
        if(d->dirty) {
            static const char* options[] = {"Save", "Discard", "Keep editing"};
            static u32 buttons[] = {KEY_A, KEY_X, KEY_B};
            d->closing = true;
            prompt_display_multi_choice("Edit the theme", _("Save the changes to this theme?"), COLOR_TEXT, options, buttons, 3, d, NULL, te_leave_answer);
        } else {
            te_close(d);
        }

        return;
    }

    if(selected == NULL) {
        return;
    }

    u32 row = (u32) (uintptr_t) selected->data;
    if(selectedTouched || (hidKeysDown() & KEY_A)) {
        te_change(d, row, 0);
    } else if(keys_hold_repeat(&d->holdAt[0], KEY_LEFT)) {
        te_change(d, row, -1);
    } else if(keys_hold_repeat(&d->holdAt[1], KEY_RIGHT)) {
        te_change(d, row, 1);
    }
}

void themeedit_open() {
    themeedit_data* d = (themeedit_data*) calloc(1, sizeof(themeedit_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate theme editor data.");
        return;
    }

    string_copy(d->id, "edit", sizeof(d->id));
    d->night = theme()->night;
    d->view = list_display(_("Edit the theme"), _(" change    back"), d, te_update, te_draw_top);
    if(d->view == NULL) {
        free(d);
        return;
    }

    list_style style = {.drawRow = te_draw_row};
    list_set_style(d->view, &style);
    list_set_filter(d->view, te_visible);
    list_set_side_keys(d->view, true);
    list_show_title_bar(d->view);
    te_load(d, settings_get()->theme);
}
