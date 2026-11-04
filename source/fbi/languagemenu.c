#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"

// Settings -> Language: every language in a list, each in its own name. A or a
// tap picks it (the whole app changes at once) and goes back; the top screen
// shows the highlighted one with a few words in it before choosing.

static const char* languagemenu_sample_words[] = {
        "Main menu",
        "Settings",
        "Titles",
        "Remote install",
        "Please wait...",
};

static language_id languagemenu_of(const list_item* item) {
    return (language_id) (uintptr_t) item->data;
}

static void languagemenu_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    language_id language = languagemenu_of(item);
    bool current = language == settings_get()->language;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    screen_draw_icon(ICON_LANGUAGE, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);
    float tx = x + 33;
    float right = x + width - 9;

    // The one in use is ticked.
    if(current) {
        screen_draw_icon(ICON_CHECK, right - 16, y + (float) (int) ((height - 16) / 2), 16, selected ? t->onAccent : t->accent);
        right -= 22;
    }

    float textY = y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    const char* name = i18n_language_name(language);
    widget_text_fit(name, tx, textY, WIDGET_TEXT_SCALE, ink, right - tx);

    // System says which one it is now.
    if(language == LANGUAGE_SYSTEM) {
        const char* now = i18n_language_name(i18n_system_language());
        float nameW = widget_text_width(name, WIDGET_TEXT_SCALE);
        float nowW = widget_text_width(now, WIDGET_SMALL_SCALE);
        if(tx + nameW + 12 + nowW <= right) {
            screen_draw_text(now, right - nowW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, muted);
        }
    }
}

static void languagemenu_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL) {
        return;
    }

    const theme_current* t = theme();
    language_id language = languagemenu_of(selected);
    language_id shown = language == LANGUAGE_SYSTEM ? i18n_system_language() : language;

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 40 + (float) (sizeof(languagemenu_sample_words) / sizeof(*languagemenu_sample_words)) * 26 + 14;
    widget_card(cardX, cardY, cardW, cardH);

    float textX = cardX + 16;
    float right = cardX + cardW - 14;
    widget_text_fit(i18n_language_name(language), textX, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text, right - textX);

    // A few words of the app in it.
    float y = cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 10;
    for(u32 i = 0; i < sizeof(languagemenu_sample_words) / sizeof(*languagemenu_sample_words); i++) {
        widget_text_fit(i18n_translate_as(shown, languagemenu_sample_words[i]), textX, y, WIDGET_TEXT_SCALE, t->palette->textMuted, right - textX);
        y += 26;
    }
}

static void languagemenu_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    view->name = _("Language");
    view->info = _("\uE000 choose   \uE001 back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();

        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            free(linked_list_iter_next(&iter));
            linked_list_iter_remove(&iter);
        }

        list_destroy(view);
        return;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < LANGUAGE_COUNT; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, i18n_language_name((language_id) i), sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) i;
                linked_list_add(items, item);

                if(i == settings_get()->language) {
                    list_select(view, item);
                }
            }
        }
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        settings_get()->language = languagemenu_of(selected);
        settings_apply();

        Result res = settings_save();
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, _("Failed to save settings."));
        }

        ui_pop();

        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            free(linked_list_iter_next(&iter));
            linked_list_iter_remove(&iter);
        }

        list_destroy(view);
    }
}

void languagemenu_open() {
    ui_view* view = list_display(_("Language"), _("\uE000 choose   \uE001 back"), NULL, languagemenu_update, languagemenu_draw_top);
    if(view == NULL) {
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = languagemenu_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}
