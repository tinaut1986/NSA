#pragma once

typedef struct __sFILE FILE;

#define TOP_SCREEN_WIDTH 400
#define TOP_SCREEN_HEIGHT 240

#define BOTTOM_SCREEN_WIDTH 320
#define BOTTOM_SCREEN_HEIGHT 240

#define MAX_TEXTURES 1024
#define MAX_COLORS 32

#define COLOR_TEXT 0

void screen_init();
void screen_exit();
void screen_set_base_alpha(u8 alpha);
void screen_set_color(u32 id, u32 color);
u32 screen_get_color(u32 id);
u32 screen_allocate_free_texture();
void screen_load_texture_untiled(u32 id, void* data, u32 size, u32 width, u32 height, GPU_TEXCOLOR format, bool linearFilter);
void screen_load_texture_path(u32 id, const char* path, bool linearFilter);
void screen_load_texture_file(u32 id, FILE* fd, bool linearFilter);
void screen_load_texture_tiled(u32 id, void* data, u32 size, u32 width, u32 height, GPU_TEXCOLOR format, bool linearFilter);
void screen_unload_texture(u32 id);
void screen_get_texture_size(u32* width, u32* height, u32 id);
void screen_begin_frame();
void screen_end_frame();
void screen_select(gfxScreen_t screen);
// The top screen in 3D: one eye, whose drawings move `shift` pixels per unit
// of depth (screen_set_depth; 0 is the screen's plane).
void screen_select_eye(gfxScreen_t screen, gfx3dSide_t side, float shift);
// The eye the top screen is being drawn for (GFX_LEFT too in 2D and on the bottom screen).
gfx3dSide_t screen_get_eye();
void screen_set_depth(float value);
float screen_get_depth();
// Moves and fades only text, icons and images drawn after it (not shapes);
// reset by screen_select.
void screen_set_content_motion(float dx, float dy, u8 alpha);
void screen_get_content_motion(float* dx, float* dy, u8* alpha);
// Leaves out shapes (fills, frames, circles) so only contents are drawn: for
// what a card held a moment ago, fading out over what it holds now.
void screen_set_shapes_hidden(bool hidden);
bool screen_get_shapes_hidden();
// Moves everything drawn after it (animations); reset by screen_select.
void screen_set_offset(float x, float y);
void screen_draw_texture(u32 id, float x, float y, float width, float height);
void screen_draw_texture_crop(u32 id, float x, float y, float width, float height);
float screen_get_font_height(float scaleY);
void screen_get_string_size(float* width, float* height, const char* text, float scaleX, float scaleY);
void screen_get_string_size_wrap(float* width, float* height, const char* text, float scaleX, float scaleY, float wrapWidth);
void screen_draw_string(const char* text, float x, float y, float scaleX, float scaleY, u32 colorId, bool centerLines);
void screen_draw_string_wrap(const char* text, float x, float y, float scaleX, float scaleY, u32 colorId, bool centerLines, float wrapX);

// Shapes and text in any colour (0xAABBGGRR, like the colour IDs), for the
// themed widgets. All honour screen_set_base_alpha.
// Writes what the screen shows to a 24-bit BMP. Main thread, between frames.
bool screen_capture(gfxScreen_t screen, const char* path);

// Clips drawing to a rectangle of the current screen until screen_clear_clip.
void screen_set_clip(float x, float y, float width, float height);
void screen_clear_clip();
// Narrows the clip to a rectangle inside it for a moment; screen_restore_clip
// goes back to the one screen_set_clip set (or none).
void screen_clip_within(float x, float y, float width, float height);
void screen_restore_clip();
void screen_fill_rect(float x, float y, float width, float height, u32 color);
void screen_fill_round_rect(float x, float y, float width, float height, float radius, u32 color);
// A rounded rectangle filled with fillColor inside a `border` px edge of borderColor.
void screen_frame_round_rect(float x, float y, float width, float height, float radius, float border, u32 borderColor, u32 fillColor);
void screen_fill_circle(float cx, float cy, float radius, u32 color);
// An icon from romfs:/icons.png (icons.h), drawn in `size` px and tinted.
void screen_draw_icon(u32 icon, float x, float y, float size, u32 color);
void screen_draw_text(const char* text, float x, float y, float scale, u32 color);
void screen_draw_text_wrap(const char* text, float x, float y, float scale, u32 color, float wrapX);