#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "widgets.h"
#include "../icons.h"
#include "../screen.h"
#include "../theme.h"

float widget_text_width(const char* text, float scale) {
    float width = 0;
    screen_get_string_size(&width, NULL, text, scale, scale);
    return width;
}

float widget_text_height(float scale) {
    return screen_get_font_height(scale);
}

void widget_text_fit(const char* text, float x, float y, float scale, u32 color, float maxWidth) {
    if(text == NULL) {
        return;
    }

    if(widget_text_width(text, scale) <= maxWidth) {
        screen_draw_text(text, x, y, scale, color);
        return;
    }

    char buf[256];
    size_t len = strlen(text);
    if(len >= sizeof(buf) - 4) {
        len = sizeof(buf) - 4;
    }

    // Shorten until it fits, keeping whole UTF-8 characters.
    while(len > 0) {
        len--;
        while(len > 0 && (text[len] & 0xC0) == 0x80) {
            len--;
        }

        memcpy(buf, text, len);
        strcpy(buf + len, "...");
        if(widget_text_width(buf, scale) <= maxWidth) {
            break;
        }
    }

    screen_draw_text(buf, x, y, scale, color);
}

void widget_text_centered(const char* text, float x1, float x2, float y, float scale, u32 color) {
    float width = widget_text_width(text, scale);
    screen_draw_text(text, x1 + (x2 - x1 - width) / 2, y, scale, color);
}

void widget_background(float width, float height) {
    screen_fill_rect(0, 0, width, height, theme()->palette->background);
}

void widget_card(float x, float y, float width, float height) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, width, height, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
}

void widget_badge(float x, float y, const char* text, u32 background, u32 foreground) {
    float width = widget_text_width(text, WIDGET_SMALL_SCALE);
    float height = widget_text_height(WIDGET_SMALL_SCALE) + 2;
    screen_fill_round_rect(x, y, width + 12, height, 6, background);
    screen_draw_text(text, x + 6, y + 1, WIDGET_SMALL_SCALE, foreground);
}

void widget_button(float x, float y, float width, float height, int icon, const char* label, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float textHeight = widget_text_height(WIDGET_TEXT_SCALE);
    float tx = x + 9;
    if(icon >= 0) {
        float size = 16;
        screen_draw_icon((u32) icon, tx, y + (height - size) / 2, size, ink);
        tx += size + 6;
    }

    // A long label gets the smaller size before it is cut.
    float room = x + width - 6 - tx;
    float scale = widget_text_width(label, WIDGET_TEXT_SCALE) <= room ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    textHeight = widget_text_height(scale);
    widget_text_fit(label, tx, y + (height - textHeight) / 2, scale, ink, room);
}

void widget_row(float x, float y, float width, float height, int icon, const char* label, u32 labelColor, const char* value, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : labelColor;
    u32 muted = selected ? t->onAccent : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float textHeight = widget_text_height(WIDGET_TEXT_SCALE);
    float ty = y + (height - textHeight) / 2;
    float tx = x + 9;
    if(icon >= 0) {
        float size = 16;
        screen_draw_icon((u32) icon, tx, y + (height - size) / 2, size, ink);
        tx += size + 6;
    }

    float right = x + width - 8;
    if(value != NULL) {
        float arrow = 12;
        float valueWidth = widget_text_width(value, WIDGET_SMALL_SCALE);
        float valueHeight = widget_text_height(WIDGET_SMALL_SCALE);
        screen_draw_icon(ICON_CHEVRON_RIGHT, right - arrow, y + (height - arrow) / 2, arrow, muted);
        float vx = right - arrow - 2 - valueWidth;
        screen_draw_text(value, vx, y + (height - valueHeight) / 2, WIDGET_SMALL_SCALE, muted);
        screen_draw_icon(ICON_CHEVRON_LEFT, vx - 2 - arrow, y + (height - arrow) / 2, arrow, muted);
        right = vx - 2 - arrow - 6;
    }

    widget_text_fit(label, tx, ty, WIDGET_TEXT_SCALE, ink, right - tx);
}

void widget_progress(float x, float y, float width, float height, float progress) {
    const theme_current* t = theme();
    if(progress < 0) {
        progress = 0;
    } else if(progress > 1) {
        progress = 1;
    }

    screen_fill_round_rect(x, y, width, height, height / 2, t->palette->border);
    if(progress > 0) {
        float filled = width * progress;
        screen_fill_round_rect(x, y, filled < height ? height : filled, height, height / 2, t->accent);
    }
}

void widget_scrollbar(float x, float y, float height, float offset, float visible, float total) {
    if(total <= visible || total <= 0) {
        return;
    }

    const theme_current* t = theme();
    float w = WIDGET_SCROLLBAR_WIDTH;
    float thumb = height * visible / total;
    if(thumb < 12) {
        thumb = 12;
    }

    float maxOffset = total - visible;
    float ty = y + (height - thumb) * (offset < 0 ? 0 : offset > maxOffset ? 1 : offset / maxOffset);
    screen_fill_round_rect(x, y, w, height, w / 2, t->palette->border);
    screen_fill_round_rect(x, ty, w, thumb, w / 2, t->accent);
}

void widget_toast(const char* text, float progress, float bottom) {
    const theme_current* t = theme();
    float textHeight = widget_text_height(WIDGET_SMALL_SCALE);
    float height = textHeight + (progress >= 0 ? 18 : 10);
    float x = 8;
    float width = BOTTOM_SCREEN_WIDTH - 16;
    float y = bottom - 6 - height;

    u32 ink = t->onAccent;
    screen_fill_round_rect(x, y, width, height, t->def->radiusSmall, t->night ? t->accent : t->palette->text);
    if(!t->night) {
        ink = t->palette->surface;
    }

    widget_text_fit(text, x + 10, y + 5, WIDGET_SMALL_SCALE, ink, width - 20);
    if(progress >= 0) {
        float barY = y + height - 9;
        screen_fill_round_rect(x + 10, barY, width - 20, 4, 2, THEME_ALPHA(ink, 0x50));
        screen_fill_round_rect(x + 10, barY, (width - 20) * (progress > 1 ? 1 : progress), 4, 2, ink);
    }
}

void widget_modal(float screenWidth, float screenHeight, float width, float height, float* x, float* y) {
    const theme_current* t = theme();
    screen_fill_rect(0, 0, screenWidth, screenHeight, t->palette->scrim);

    *x = (screenWidth - width) / 2;
    *y = (screenHeight - height) / 2;
    screen_frame_round_rect(*x, *y, width, height, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
}

bool widget_touched(const touchPosition* pos, float x, float y, float width, float height) {
    return pos->px >= x && pos->px < x + width && pos->py >= y && pos->py < y + height;
}
