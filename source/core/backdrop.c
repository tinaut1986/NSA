#include <math.h>
#include <string.h>

#include <3ds.h>

#include "backdrop.h"
#include "screen.h"
#include "theme.h"
#include "xferturn.h"

// Each particle has a depth behind the screen's plane (screen_set_depth: negative is behind) from BACKDROP_NEAR to
// BACKDROP_FAR; the far ones are smaller, slower and fainter, so it reads as depth even without the 3D slider.
#define BACKDROP_MAX 72
#define BACKDROP_NEAR -1.0f
#define BACKDROP_FAR -6.0f
#define BACKDROP_PI 3.14159265f

typedef struct {
    float x;
    float y;
    float depth;
    float far;      // 0 at BACKDROP_NEAR, 1 at BACKDROP_FAR.
    float size;
    float speed;
    float phase;
    float angle;    // Fireflies: where it is heading.
} backdrop_particle;

static backdrop_style backdrop_style_now = BACKDROP_OFF;
static backdrop_particle backdrop_particles[BACKDROP_MAX];
static u32 backdrop_count = 0;
static u32 backdrop_seed = 1;
static float backdrop_time = 0;
static u64 backdrop_last = 0;
static float backdrop_width = TOP_SCREEN_WIDTH;
static float backdrop_height = TOP_SCREEN_HEIGHT;

static float backdrop_random() {
    u32 x = backdrop_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    backdrop_seed = x;
    return (float) (x >> 8) / (float) (1 << 24);
}

static u32 backdrop_count_of(backdrop_style style) {
    switch(style) {
        case BACKDROP_BUBBLES: return 40;
        case BACKDROP_SNOW: return 56;
        case BACKDROP_STARS: return BACKDROP_MAX;
        case BACKDROP_FIREFLIES: return 22;
        default: return 0;
    }
}

// A particle anywhere on the screen (`anywhere`), or entering it from the edge its movement comes from.
static void backdrop_spawn(backdrop_particle* p, bool anywhere) {
    p->far = backdrop_random();
    p->depth = BACKDROP_NEAR + (BACKDROP_FAR - BACKDROP_NEAR) * p->far;
    p->phase = backdrop_random() * 2 * BACKDROP_PI;
    p->angle = backdrop_random() * 2 * BACKDROP_PI;
    p->x = backdrop_random() * backdrop_width;
    p->y = backdrop_random() * backdrop_height;
    float near = 1.0f - p->far;
    switch(backdrop_style_now) {
        case BACKDROP_BUBBLES:
            p->size = (2.5f + 5.5f * backdrop_random()) * (0.55f + 0.45f * near);
            p->speed = (10.0f + 18.0f * backdrop_random()) * (0.5f + 0.5f * near);
            if(!anywhere) {
                p->y = backdrop_height + p->size + 2;
            }

            break;
        case BACKDROP_SNOW:
            p->size = (1.3f + 2.0f * backdrop_random()) * (0.6f + 0.4f * near);
            p->speed = (8.0f + 14.0f * backdrop_random()) * (0.45f + 0.55f * near);
            if(!anywhere) {
                p->y = -p->size - 2;
            }

            break;
        case BACKDROP_STARS:
            p->size = (1.0f + 1.6f * backdrop_random() * backdrop_random()) * (0.65f + 0.35f * near);
            p->speed = (0.6f + 2.4f * near) * (0.6f + 0.4f * backdrop_random());
            if(!anywhere) {
                p->x = backdrop_width + 3;
            }

            break;
        case BACKDROP_FIREFLIES:
            p->size = (1.3f + 1.2f * backdrop_random()) * (0.65f + 0.35f * near);
            p->speed = (7.0f + 9.0f * backdrop_random()) * (0.6f + 0.4f * near);
            break;
        default:
            break;
    }
}

void backdrop_set(backdrop_style style) {
    if(style >= BACKDROP_COUNT) {
        style = BACKDROP_OFF;
    }

    if(style == backdrop_style_now) {
        return;
    }

    backdrop_style_now = style;
    backdrop_seed = (u32) osGetTime() ^ 0x9E3779B9u;
    if(backdrop_seed == 0) {
        backdrop_seed = 1;
    }

    backdrop_count = backdrop_count_of(style);
    for(u32 i = 0; i < backdrop_count; i++) {
        backdrop_spawn(&backdrop_particles[i], true);
    }

    backdrop_last = 0;
}

// Still while a transfer has the turn: nothing is drawn for it then, and the transfer keeps the CPU.
static bool backdrop_paused() {
    xferturn_status status;
    xferturn_get(&status);
    return status.busy;
}

bool backdrop_enabled() {
    return backdrop_style_now != BACKDROP_OFF;
}

bool backdrop_active() {
    return backdrop_style_now != BACKDROP_OFF && !backdrop_paused();
}

void backdrop_update() {
    if(backdrop_style_now == BACKDROP_OFF) {
        return;
    }

    u64 now = osGetTime();
    float dt = backdrop_last == 0 ? 0 : (float) (now - backdrop_last) / 1000.0f;
    backdrop_last = now;
    if(backdrop_paused()) {
        return;
    }

    // After a pause (a transfer, NSA in the background) it goes on from where it was.
    if(dt > 0.1f) {
        dt = 0.1f;
    }

    backdrop_time += dt;
    for(u32 i = 0; i < backdrop_count; i++) {
        backdrop_particle* p = &backdrop_particles[i];
        switch(backdrop_style_now) {
            case BACKDROP_BUBBLES:
                p->y -= p->speed * dt;
                if(p->y < -p->size - 2) {
                    backdrop_spawn(p, false);
                }

                break;
            case BACKDROP_SNOW: {
                // A wind that turns slowly, the same for all, a little different for each flake.
                float wind = 6.0f * sinf(backdrop_time * 0.25f) + 3.0f * sinf(backdrop_time * 1.3f + p->phase);
                p->y += p->speed * dt;
                p->x += wind * (1.0f - 0.5f * p->far) * dt;
                if(p->x < -4) {
                    p->x += backdrop_width + 8;
                } else if(p->x > backdrop_width + 4) {
                    p->x -= backdrop_width + 8;
                }

                if(p->y > backdrop_height + p->size + 2) {
                    backdrop_spawn(p, false);
                }

                break;
            }
            case BACKDROP_STARS:
                p->x -= p->speed * dt;
                if(p->x < -3) {
                    backdrop_spawn(p, false);
                }

                break;
            case BACKDROP_FIREFLIES:
                // A slow wander: the heading turns one way and the other.
                p->angle += 1.4f * sinf(backdrop_time * 0.7f + p->phase) * dt;
                p->x += cosf(p->angle) * p->speed * dt;
                p->y += sinf(p->angle) * p->speed * dt;
                if(p->x < -6 || p->x > backdrop_width + 6) {
                    p->angle = BACKDROP_PI - p->angle;
                    p->x = p->x < 0 ? -6 : backdrop_width + 6;
                }

                if(p->y < -6 || p->y > backdrop_height + 6) {
                    p->angle = -p->angle;
                    p->y = p->y < 0 ? -6 : backdrop_height + 6;
                }

                break;
            default:
                break;
        }
    }
}

static u32 backdrop_color(u32 rgb, float alpha) {
    if(alpha < 0) {
        alpha = 0;
    } else if(alpha > 1) {
        alpha = 1;
    }

    return ((u32) (alpha * 255.0f) << 24) | (rgb & 0x00FFFFFF);
}

void backdrop_draw(float width, float height) {
    if(backdrop_style_now == BACKDROP_OFF) {
        return;
    }

    backdrop_width = width;
    backdrop_height = height;
    u32 accent = theme()->accent;
    float before = screen_get_depth();
    for(u32 i = 0; i < backdrop_count; i++) {
        backdrop_particle* p = &backdrop_particles[i];
        float near = 1.0f - p->far;
        screen_set_depth(p->depth);
        switch(backdrop_style_now) {
            case BACKDROP_BUBBLES: {
                // A see-through ball with a glint, swaying as it rises.
                float x = p->x + 5.0f * (0.4f + 0.6f * near) * sinf(p->phase + p->y / 38.0f);
                screen_fill_circle(x, p->y, p->size, backdrop_color(accent, 0.16f + 0.16f * near));
                screen_fill_circle(x, p->y, p->size * 0.72f, backdrop_color(accent, 0.08f + 0.08f * near));
                screen_fill_circle(x - p->size * 0.35f, p->y - p->size * 0.35f, p->size * 0.26f, backdrop_color(0xFFFFFF, 0.35f + 0.25f * near));
                break;
            }
            case BACKDROP_SNOW:
                screen_fill_circle(p->x, p->y, p->size, backdrop_color(accent, 0.45f + 0.45f * near));
                break;
            case BACKDROP_STARS: {
                float twinkle = 0.5f + 0.5f * sinf(backdrop_time * (1.2f + 2.0f * p->speed / 3.0f) + p->phase);
                float alpha = (0.45f + 0.50f * near) * (0.45f + 0.55f * twinkle);
                screen_fill_circle(p->x, p->y, p->size, backdrop_color(accent, alpha));
                // The bigger ones flash a little cross at their brightest.
                if(p->size > 1.4f && twinkle > 0.75f) {
                    float arm = p->size * 2.4f;
                    u32 color = backdrop_color(accent, alpha * 0.6f);
                    screen_fill_rect(p->x - arm, p->y - 0.5f, arm * 2, 1, color);
                    screen_fill_rect(p->x - 0.5f, p->y - arm, 1, arm * 2, color);
                }

                break;
            }
            case BACKDROP_FIREFLIES: {
                // A glow that swells and fades, with a bright core.
                float glow = 0.5f + 0.5f * sinf(backdrop_time * (0.9f + 0.05f * p->speed) + p->phase);
                screen_fill_circle(p->x, p->y, p->size * 3.2f, backdrop_color(accent, (0.08f + 0.08f * near) * glow));
                screen_fill_circle(p->x, p->y, p->size * 1.8f, backdrop_color(accent, (0.15f + 0.15f * near) * glow));
                screen_fill_circle(p->x, p->y, p->size, backdrop_color(accent, (0.55f + 0.40f * near) * (0.35f + 0.65f * glow)));
                break;
            }
            default:
                break;
        }
    }

    screen_set_depth(before);
}
