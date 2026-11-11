// The theme editor: a list of the theme's values (list.c, Left and Right change the selected one), its colours picked in
// colorpick.c. While it is open everything is drawn with the copy being edited (theme_preview), in the palette whose
// colours show (day or night).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "../core/core.h"
#include "colorpick.h"
#include "settings.h"
#include "themeedit.h"

typedef enum {
    TE_NAME,
    TE_PALETTE,
    TE_COLOR_FIRST,
    TE_COLOR_LAST = TE_COLOR_FIRST + 9,
    TE_RADIUS,
    TE_RADIUS_SMALL,
    TE_BORDER,
    TE_FONT,
    TE_BARS,
    TE_DIALOGS,
    TE_MENU,
    TE_GRID,
    TE_SAVE,
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
    u32 editing;          // The theme's index (a built-in one is saved as a new file).
    bool night;           // Which palette the colour rows show and the screens are drawn in.
    bool dirty;
    bool closing;
    u32 pickingRow;
} themeedit_data;

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

static const char* te_label(u32 row) {
    switch(row) {
        case TE_NAME: return "Name";
        case TE_PALETTE: return "Colours of";
        case TE_RADIUS: return "Corners";
        case TE_RADIUS_SMALL: return "Corners of rows and buttons";
        case TE_BORDER: return "Outline width";
        case TE_FONT: return "Font";
        case TE_BARS: return "Progress bars";
        case TE_DIALOGS: return "Dialogs";
        case TE_MENU: return "Main menu";
        case TE_GRID: return "Icon grids";
        case TE_SAVE: return "Save the theme";
        default: return te_colors[row - TE_COLOR_FIRST].label;
    }
}

// The row's value, translated; NULL for the rows that open something.
static const char* te_value(themeedit_data* d, u32 row, char* buf, size_t size) {
    switch(row) {
        case TE_NAME: return d->name;
        case TE_PALETTE: return d->night ? _("Night") : _("Day");
        case TE_RADIUS: snprintf(buf, size, "%d", (int) d->def.radius); return buf;
        case TE_RADIUS_SMALL: snprintf(buf, size, "%d", (int) d->def.radiusSmall); return buf;
        case TE_BORDER: snprintf(buf, size, "%d", (int) d->def.border); return buf;
        case TE_FONT: return d->def.pixelFont ? _("Pixel") : _("System");
        case TE_BARS: return d->def.blockBars ? _("Blocks") : _("Smooth");
        case TE_DIALOGS: return d->def.sheetPrompts ? _("Sheet from below") : _("Window");
        case TE_MENU: return d->def.menuLayout == THEME_MENU_TILES ? _("Big tiles") : _("Buttons");
        case TE_GRID: return d->def.pagedGrids ? _("Pages sideways") : _("Scroll down");
        default: return NULL;
    }
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
            [TE_NAME] = ICON_PENCIL, [TE_PALETTE] = ICON_SUN_MOON, [TE_RADIUS] = ICON_SQUARE, [TE_RADIUS_SMALL] = ICON_SQUARE,
            [TE_BORDER] = ICON_SQUARE, [TE_FONT] = ICON_ABC, [TE_BARS] = ICON_HOURGLASS, [TE_DIALOGS] = ICON_APP_WINDOW,
            [TE_MENU] = ICON_LAYOUT_GRID, [TE_GRID] = ICON_LAYOUT_GRID, [TE_SAVE] = ICON_DEVICE_FLOPPY
    };
    const char* value = te_value(d, row, buf, sizeof(buf));
    if(row == TE_NAME) {
        // A name is typed, not stepped through: no arrows.
        widget_row(x, y, width, height, icons[row], i18n_translate_text(te_label(row)), t->palette->text, NULL, selected);
        float right = widget_row_chevron(x + width - 8, y, height, selected);
        float tw = widget_text_width(value, WIDGET_SMALL_SCALE);
        float vx = right - tw;
        screen_draw_text(value, vx > x + width / 2 ? vx : x + width / 2, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2,
                         WIDGET_SMALL_SCALE, selected ? t->onAccent : t->palette->textMuted);
        return;
    }

    widget_row(x, y, width, height, icons[row], i18n_translate_text(te_label(row)), row == TE_SAVE && !selected ? t->accent : t->palette->text, value, selected);
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

static void te_close(themeedit_data* d) {
    ui_view* view = d->view;
    theme_preview_end();
    ui_pop();

    linked_list* items = list_get_items(view);
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }

    list_destroy(view);
    free(d);
}

static bool te_save(themeedit_data* d) {
    settings_data* settings = settings_get();
    char path[256];
    u32 saved = 0;
    if(!theme_save_custom(&d->def, d->name, d->editing, &saved, path, sizeof(path))) {
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
    // 0 saves the changes, 1 throws them away, 2 (B) keeps editing.
    if(response == 0) {
        te_save(d);
    } else if(response == 1) {
        te_close(d);
    } else {
        d->closing = false;
    }
}

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
        case TE_NAME:
            if(step == 0) {
                osk_display(_("Name of the theme"), d->name, sizeof(d->name) - 1, 0, d, te_name_typed);
            }
            return;
        case TE_PALETTE:
            d->night = !d->night;
            te_preview(d);
            return;
        case TE_RADIUS: te_step_number(&d->def.radius, step, 24); break;
        case TE_RADIUS_SMALL: te_step_number(&d->def.radiusSmall, step, 24); break;
        case TE_BORDER: te_step_number(&d->def.border, step, 4); break;
        case TE_FONT: if(turn) d->def.pixelFont = !d->def.pixelFont; break;
        case TE_BARS: if(turn) d->def.blockBars = !d->def.blockBars; break;
        case TE_DIALOGS: if(turn) d->def.sheetPrompts = !d->def.sheetPrompts; break;
        case TE_MENU: if(turn) d->def.menuLayout = d->def.menuLayout == THEME_MENU_TILES ? THEME_MENU_GRID2 : THEME_MENU_TILES; break;
        case TE_GRID: if(turn) d->def.pagedGrids = !d->def.pagedGrids; break;
        case TE_SAVE:
            if(step == 0) {
                te_save(d);
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
    } else if(hidKeysDown() & KEY_LEFT) {
        te_change(d, row, -1);
    } else if(hidKeysDown() & KEY_RIGHT) {
        te_change(d, row, 1);
    }
}

void themeedit_open() {
    themeedit_data* d = (themeedit_data*) calloc(1, sizeof(themeedit_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate theme editor data.");
        return;
    }

    settings_data* settings = settings_get();
    d->editing = settings->theme;
    d->def = *theme_get(d->editing);
    // A built-in theme is not changed: what is edited is a copy, saved as a theme of one's own.
    if(theme_is_custom(d->editing)) {
        string_copy(d->name, d->def.name, sizeof(d->name));
    } else {
        snprintf(d->name, sizeof(d->name), "%s 2", d->def.name);
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
    list_set_side_keys(d->view, true);
    list_show_title_bar(d->view);
    te_preview(d);
}
