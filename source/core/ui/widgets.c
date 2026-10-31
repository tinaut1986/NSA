#include <math.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "ui.h"
#include "widgets.h"
#include "../i18n.h"
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

    // One line: titles' descriptions often carry line breaks.
    char buf[256];
    size_t len = strlen(text);
    if(len >= sizeof(buf) - 4) {
        len = sizeof(buf) - 4;
    }

    for(size_t i = 0; i < len; i++) {
        buf[i] = text[i] == '\n' || text[i] == '\r' ? ' ' : text[i];
    }

    buf[len] = '\0';

    if(widget_text_width(buf, scale) <= maxWidth) {
        screen_draw_text(buf, x, y, scale, color);
        return;
    }

    // Shorten until it fits, keeping whole UTF-8 characters.
    char cut[256];
    while(len > 0) {
        len--;
        while(len > 0 && (buf[len] & 0xC0) == 0x80) {
            len--;
        }

        memcpy(cut, buf, len);
        strcpy(cut + len, "...");
        if(widget_text_width(cut, scale) <= maxWidth) {
            break;
        }
    }

    screen_draw_text(cut, x, y, scale, color);
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

// The colours of a button: the accent, or a soft red for what is dangerous.
static void widget_button_colors(bool selected, bool danger, u32* fill, u32* edge, u32* ink) {
    const theme_current* t = theme();
    if(danger) {
        u32 onDanger = t->night ? t->palette->background : t->onAccent;
        *fill = selected ? t->palette->danger : theme_mix(t->palette->surface, t->palette->danger, 0x26);
        *edge = t->palette->danger;
        *ink = selected ? onDanger : t->palette->danger;
    } else {
        *fill = selected ? t->accent : t->palette->surface;
        *edge = selected ? t->accent : t->palette->border;
        *ink = selected ? t->onAccent : t->palette->text;
    }
}

static void widget_button_draw(float x, float y, float width, float height, int icon, const char* label, bool selected, bool danger) {
    const theme_current* t = theme();
    u32 fill, edge, ink;
    widget_button_colors(selected, danger, &fill, &edge, &ink);
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

void widget_button(float x, float y, float width, float height, int icon, const char* label, bool selected) {
    widget_button_draw(x, y, width, height, icon, label, selected, false);
}

void widget_button_danger(float x, float y, float width, float height, int icon, const char* label, bool selected) {
    widget_button_draw(x, y, width, height, icon, label, selected, true);
}

void widget_button_centered_danger(float x, float y, float width, float height, const char* label, bool selected) {
    const theme_current* t = theme();
    u32 fill, edge, ink;
    widget_button_colors(selected, true, &fill, &edge, &ink);
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float scale = widget_text_width(label, WIDGET_TEXT_SCALE) <= width - 12 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_centered(label, x, x + width, y + (height - widget_text_height(scale)) / 2, scale, ink);
}

void widget_button_centered(float x, float y, float width, float height, const char* label, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float scale = widget_text_width(label, WIDGET_TEXT_SCALE) <= width - 12 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_centered(label, x, x + width, y + (height - widget_text_height(scale)) / 2, scale, ink);
}

// A row whose box is `frameHeight` tall and whose selector line is `height` tall at its top.
static void widget_row_draw(float x, float y, float width, float frameHeight, float height, int icon, const char* label, u32 labelColor, const char* value, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : labelColor;
    u32 muted = selected ? t->onAccent : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, frameHeight, t->def->radiusSmall, t->def->border, edge, fill);

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

void widget_row(float x, float y, float width, float height, int icon, const char* label, u32 labelColor, const char* value, bool selected) {
    widget_row_draw(x, y, width, height, height, icon, label, labelColor, value, selected);
}

float widget_row_slider_height() {
    return WIDGET_ROW_HEIGHT + WIDGET_SLIDER_EXTRA;
}

void widget_row_slider_track(float x, float y, float width, float* trackX, float* trackWidth, float* centerY) {
    *trackX = x + 12;
    *trackWidth = width - 12 - 46;
    *centerY = y + WIDGET_ROW_HEIGHT + WIDGET_SLIDER_EXTRA / 2 - 1;
}

void widget_row_with_slider(float x, float y, float width, int icon, const char* label, u32 labelColor, const char* value, bool selected, float fraction, const char* sliderText) {
    const theme_current* t = theme();
    widget_row_draw(x, y, width, widget_row_slider_height(), WIDGET_ROW_HEIGHT, icon, label, labelColor, value, selected);

    float trackX, trackWidth, centerY;
    widget_row_slider_track(x, y, width, &trackX, &trackWidth, &centerY);
    if(fraction < 0) {
        fraction = 0;
    } else if(fraction > 1) {
        fraction = 1;
    }

    // The track and what is filled, then a knob where a finger can take it.
    u32 track = selected ? THEME_ALPHA(t->onAccent, 0x55) : t->palette->border;
    u32 filled = selected ? t->onAccent : t->accent;
    screen_fill_round_rect(trackX, centerY - 3, trackWidth, 6, 3, track);
    if(fraction > 0) {
        screen_fill_round_rect(trackX, centerY - 3, trackWidth * fraction, 6, 3, filled);
    }

    float knobX = trackX + trackWidth * fraction;
    screen_fill_round_rect(knobX - 4, centerY - 7, 8, 14, 4, filled);

    if(sliderText != NULL) {
        float w = widget_text_width(sliderText, WIDGET_SMALL_SCALE);
        screen_draw_text(sliderText, x + width - 8 - w, centerY - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, selected ? t->onAccent : t->palette->textMuted);
    }
}


void widget_progress(float x, float y, float width, float height, float progress) {
    const theme_current* t = theme();
    if(progress < 0) {
        progress = 0;
    } else if(progress > 1) {
        progress = 1;
    }

    if(t->def->blockBars) {
        // Blocks, 2 px apart, from the left: the theme's look for a bar.
        float block = height >= 8 ? 4 : 3;
        u32 count = (u32) ((width + 2) / (block + 2));
        if(count == 0) {
            count = 1;
        }

        float pitch = (width + 2) / count;
        u32 on = (u32) (count * progress + 0.5f);
        if(progress > 0 && on == 0) {
            on = 1;
        }

        for(u32 i = 0; i < count; i++) {
            screen_fill_rect((float) (int) (x + i * pitch), y, pitch - 2 >= 1 ? pitch - 2 : 1, height, i < on ? t->accent : t->palette->border);
        }

        return;
    }

    screen_fill_round_rect(x, y, width, height, height / 2, t->palette->border);
    if(progress > 0) {
        float filled = width * progress;
        screen_fill_round_rect(x, y, filled < height ? height : filled, height, height / 2, t->accent);
    }
}

bool widget_scrollbar_touched(const touchPosition* pos, float x, float y, float height, float visible, float total) {
    return total > visible && total > 0 && pos->px >= x - 16 && pos->px <= x + WIDGET_SCROLLBAR_WIDTH + 10 && pos->py >= y - 4 && pos->py <= y + height + 4;
}

float widget_scrollbar_offset(float y, float height, float visible, float total, float py) {
    float thumb = height * visible / total;
    if(thumb < 12) {
        thumb = 12;
    }

    float fraction = (py - y - thumb / 2) / (height - thumb);
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    return fraction * (total - visible);
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

// ---- presses ----

void widget_press_begin(widget_press* p, int index, bool touch, float x, float y) {
    p->index = index;
    p->touch = touch;
    p->cancelled = false;
    p->fired = false;
    p->x = x;
    p->y = y;
    p->start = osGetTime();
}

void widget_press_end(widget_press* p) {
    p->index = -1;
    p->cancelled = false;
    p->fired = false;
}

bool widget_press_on(const widget_press* p, int index) {
    return p->index >= 0 && p->index == index && !p->cancelled && !p->fired;
}

float widget_press_progress(const widget_press* p) {
    if(p->index < 0 || p->cancelled) {
        return 0;
    }

    u64 held = osGetTime() - p->start;
    if(held <= WIDGET_LONG_PRESS_DELAY) {
        return 0;
    }

    float progress = (float) (held - WIDGET_LONG_PRESS_DELAY) / (WIDGET_LONG_PRESS_TIME - WIDGET_LONG_PRESS_DELAY);
    return progress > 1 ? 1 : progress;
}

bool widget_press_long_due(widget_press* p) {
    if(p->index < 0 || p->cancelled || p->fired || widget_press_progress(p) < 1) {
        return false;
    }

    p->fired = true;
    return true;
}

void widget_press_draw(const widget_press* p, int index, float x, float y, float width, float height, float radius, u32 ink, bool fill) {
    if(!widget_press_on(p, index)) {
        return;
    }

    screen_fill_round_rect(x, y, width, height, radius, THEME_ALPHA(ink, 0x22));

    float progress = fill ? widget_press_progress(p) : 0;
    if(progress <= 0) {
        return;
    }

    // Out to the farthest corner, easing out.
    float dx = p->x - x > x + width - p->x ? p->x - x : x + width - p->x;
    float dy = p->y - y > y + height - p->y ? p->y - y : y + height - p->y;
    float reach = sqrtf(dx * dx + dy * dy);
    float eased = 1 - (1 - progress) * (1 - progress);

    screen_clip_within(x, y, width, height);
    screen_fill_circle(p->x, p->y, reach * eased, THEME_ALPHA(ink, 0x38));
    screen_restore_clip();
}

// ---- motion ----

float widget_ease(u64 start, u32 duration) {
    u64 now = osGetTime();
    if(now >= start + duration) {
        return 1;
    }

    float t = (float) (now - start) / duration;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// ---- storage ----

#define STORAGE_MEDIA 4

static struct {
    bool known;
    u64 free;
    u64 total;
    u64 time;
} storage_cache[STORAGE_MEDIA];

static void widget_size_text(char* out, size_t size, u64 bytes) {
    snprintf(out, size, "%.1f %s", ui_get_display_size(bytes), ui_get_display_size_units(bytes));
}

void widget_storage(FS_SystemMediaType media, float x, float y, float width) {
    // Drawn once, not again with a card's leaving contents.
    if((u32) media >= STORAGE_MEDIA || screen_get_shapes_hidden()) {
        return;
    }

    // Installs and deletes change it: look again every couple of seconds.
    if(!storage_cache[media].known || osGetTime() >= storage_cache[media].time + 2000) {
        FS_ArchiveResource resource = {0};
        if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, media))) {
            storage_cache[media].free = (u64) resource.freeClusters * resource.clusterSize;
            storage_cache[media].total = (u64) resource.totalClusters * resource.clusterSize;
            storage_cache[media].known = true;
        }

        storage_cache[media].time = osGetTime();
    }

    if(!storage_cache[media].known || storage_cache[media].total == 0) {
        return;
    }

    const char* name = media == SYSTEM_MEDIATYPE_SD ? _("SD card") : media == SYSTEM_MEDIATYPE_CTR_NAND ? "CTR NAND" : media == SYSTEM_MEDIATYPE_TWL_NAND ? "TWL NAND" : "TWL Photo";
    u32 icon = media == SYSTEM_MEDIATYPE_SD ? ICON_DEVICE_SD_CARD : media == SYSTEM_MEDIATYPE_TWL_PHOTO ? ICON_PHOTO : ICON_CPU;

    char freeText[32];
    char totalText[32];
    char text[96];
    widget_size_text(freeText, sizeof(freeText), storage_cache[media].free);
    widget_size_text(totalText, sizeof(totalText), storage_cache[media].total);
    snprintf(text, sizeof(text), _("%s free of %s"), freeText, totalText);

    // Not part of what changes with a selection: it stays still.
    float motionX;
    float motionY;
    u8 alpha;
    screen_get_content_motion(&motionX, &motionY, &alpha);
    screen_set_content_motion(0, 0, 0xFF);

    // Narrow: "X/Y GiB" (each with its unit when they differ: "X MiB/Y GiB"),
    // or only what is free.
    float room = width - 17 - widget_text_width(name, WIDGET_SMALL_SCALE) - 8;
    if(widget_text_width(text, WIDGET_SMALL_SCALE) > room) {
        u64 freeBytes = storage_cache[media].free;
        u64 totalBytes = storage_cache[media].total;
        const char* freeUnits = ui_get_display_size_units(freeBytes);
        const char* totalUnits = ui_get_display_size_units(totalBytes);
        if(strcmp(freeUnits, totalUnits) == 0) {
            snprintf(text, sizeof(text), "%.1f/%.1f %s", ui_get_display_size(freeBytes), ui_get_display_size(totalBytes), totalUnits);
        } else {
            snprintf(text, sizeof(text), "%s/%s", freeText, totalText);
        }

        if(widget_text_width(text, WIDGET_SMALL_SCALE) > room) {
            snprintf(text, sizeof(text), "%s", freeText);
        }
    }

    const theme_current* t = theme();
    screen_draw_icon(icon, x, y - 1, 13, t->palette->textMuted);
    screen_draw_text(name, x + 17, y, WIDGET_SMALL_SCALE, t->palette->text);
    float textW = widget_text_width(text, WIDGET_SMALL_SCALE);
    screen_draw_text(text, x + width - textW, y, WIDGET_SMALL_SCALE, t->palette->textMuted);

    float used = 1 - (float) storage_cache[media].free / storage_cache[media].total;
    widget_progress(x, y + widget_text_height(WIDGET_SMALL_SCALE) + 3, width, 5, used);

    screen_set_content_motion(motionX, motionY, alpha);
}

void widget_crossfade(float progress, void (*draw)(void* context, bool leaving), void* context, bool hasLeaving) {
    if(progress >= 1) {
        draw(context, false);
        return;
    }

    // What arrives drops in from a little above; what leaves sinks a little
    // and fades, without its boxes (they are the same ones).
    screen_set_content_motion(0, -(1 - progress) * WIDGET_CROSSFADE_DISTANCE, (u8) (progress * 0xFF));
    draw(context, false);

    if(hasLeaving) {
        screen_set_shapes_hidden(true);
        screen_set_content_motion(0, progress * WIDGET_CROSSFADE_DISTANCE, (u8) ((1 - progress) * 0xFF));
        draw(context, true);
        screen_set_shapes_hidden(false);
    }

    screen_set_content_motion(0, 0, 0xFF);
}

// ---- status ----

// Digits centred between x1 and x2 with the 1 narrower: the font's digits all
// advance the same, so "100" would show a wide gap after the 1. Each glyph is
// centred in its cell and placed on whole pixels.
static void widget_text_packed(const char* text, float x1, float x2, float y, float scale, u32 color) {
    float cells[16];
    float advances[16];
    u32 count = 0;
    float total = 0;
    for(const char* c = text; *c != '\0' && count < 16; c++, count++) {
        advances[count] = screen_get_char_width(*c, scale);
        cells[count] = *c == '1' ? advances[count] * 0.6f : advances[count];
        total += cells[count];
    }

    float cursor = x1 + (x2 - x1 - total) / 2;
    for(u32 i = 0; i < count; i++) {
        char glyph[2] = {text[i], '\0'};
        float gx = cursor + (cells[i] - advances[i]) / 2;
        screen_draw_text(glyph, (float) (int) (gx + 0.5f), (float) (int) (y + 0.5f), scale, color);
        cursor += cells[i];
    }
}

void widget_battery_state(int* percent, u8* level, bool* charging) {
    static bool known = false;
    static bool hwcTried = false;
    static bool hwc = false;
    static int lastPercent = -1;
    static u8 lastLevel = 0;
    static bool lastCharging = false;
    static u64 lastTime = 0;

    if(!known || osGetTime() >= lastTime + 2000) {
        // The exact percentage needs mcu::HWC; without it, PTM's 0..5 levels.
        if(!hwcTried) {
            hwc = R_SUCCEEDED(mcuHwcInit());
            hwcTried = true;
        }

        u8 value = 0;
        lastPercent = hwc && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&value)) ? (value > 100 ? 100 : value) : -1;

        if(R_FAILED(PTMU_GetBatteryLevel(&lastLevel))) {
            lastLevel = 0;
        }

        u8 state = 0;
        lastCharging = R_SUCCEEDED(PTMU_GetBatteryChargeState(&state)) && state;
        lastTime = osGetTime();
        known = true;
    }

    *percent = lastPercent;
    *level = lastLevel;
    *charging = lastCharging;
}

float widget_battery(float right, float y, float height) {
    const theme_current* t = theme();
    int percent;
    u8 level;
    bool charging;
    widget_battery_state(&percent, &level, &charging);

    float fraction = percent >= 0 ? percent / 100.0f : level / 5.0f;
    u32 color = fraction > 0.3f ? t->palette->success : fraction > 0.15f ? t->palette->warning : t->palette->danger;

    float nub = 2;
    float w = 28;
    float x = right - nub - w;
    // The terminal: 2 px wide and 4 tall on whole pixels (a fractional edge
    // left a stray pixel under it).
    screen_fill_rect(x + w, y + (float) (int) ((height - 4) / 2), nub, 4, color);
    screen_frame_round_rect(x, y, w, height, 3, 1, color, t->palette->surface);

    float inner = w - 4;
    if(fraction > 0) {
        screen_fill_round_rect(x + 2, y + 2, inner * fraction < 3 ? 3 : inner * fraction, height - 4, 2, theme_mix(t->palette->surface, color, 0x70));
    }

    if(percent >= 0) {
        char text[8];
        snprintf(text, sizeof(text), "%d", percent);
        // Digits 8 px tall, one pixel of battery above and below them: the
        // baseline goes on the second-last inner row. (The console's system
        // font; Azahar's stand-in sits differently.)
        float scale = 0.41f;
        float baseline = screen_get_font_baseline(scale);
        widget_text_packed(text, x, x + w, y + height - 2 - baseline, scale, t->palette->text);
    }

    float used = w + nub;
    if(charging) {
        screen_draw_icon(ICON_BOLT, x - height - 1, y, height, color);
        used += height + 1;
    }

    return used;
}

float widget_wifi(float x, float y, float height) {
    const theme_current* t = theme();
    u32 status = 0;
    bool online = R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status;
    u8 strength = online ? osGetWifiStrength() : 0;

    float barW = 3;
    float gap = 2;
    for(u32 i = 0; i < 3; i++) {
        float h = height * (i + 2) / 4;
        bool on = online && i < strength;
        screen_fill_round_rect(x + i * (barW + gap), y + height - h, barW, h, 1, on ? t->palette->text : t->palette->border);
    }

    return 3 * barW + 2 * gap;
}
