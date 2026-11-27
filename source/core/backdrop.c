#include <math.h>
#include <string.h>

#include <3ds.h>

#include "backdrop.h"
#include "screen.h"
#include "theme.h"
#include "xferturn.h"

// Each particle has a depth behind the screen's plane (screen_set_depth: negative is behind) from BACKDROP_NEAR to
// BACKDROP_FAR; the far ones are smaller, slower and fainter, so it reads as depth even without the 3D slider.
#define BACKDROP_MAX 80
#define BACKDROP_NEAR -1.0f
#define BACKDROP_FAR -6.0f
#define BACKDROP_PI 3.14159265f
// Where a moving light has been, for its tail: one point every backdrop_trail_step() seconds, at most
// backdrop_trail_max() of them (forest, threads).
#define BACKDROP_TRAIL 40

typedef struct {
    float x;
    float y;
    float depth;
    float far;      // 0 at BACKDROP_NEAR, 1 at BACKDROP_FAR.
    float size;
    float speed;
    float phase;
    float angle;    // Fireflies, forest: where it is heading.
    float vx;       // Energy: its drift.
    float base;     // Threads, energy: the x it rises from.
    float age;      // Forest, threads, energy: seconds since it appeared (negative: waiting to appear).
    float life;     // ... and how long it lasts.
    float trailTimer;
    u8 trailCount;
    u8 trailHead;   // The newest point.
    float trailX[BACKDROP_TRAIL];
    float trailY[BACKDROP_TRAIL];
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
        case BACKDROP_STARS: return 72;
        case BACKDROP_FIREFLIES: return 22;
        case BACKDROP_FOREST: return 10;
        case BACKDROP_THREADS: return 11;
        case BACKDROP_ENERGY: return 40;
        default: return 0;
    }
}

static float backdrop_trail_step() {
    return backdrop_style_now == BACKDROP_THREADS ? 0.08f : 0.06f;
}

static u8 backdrop_trail_max() {
    return backdrop_style_now == BACKDROP_THREADS ? 24 : BACKDROP_TRAIL;
}

// Energy: the glowing surface along the bottom, its height at x in pixels: several waves of their own lengths and speeds
// added up, so it swells and sinks all along without repeating soon.
static float backdrop_energy_height(float x) {
    float t = backdrop_time;
    float h = 20.0f + 9.0f * sinf(x * 0.021f + t * 0.9f) + 6.0f * sinf(x * 0.047f - t * 1.6f) + 4.0f * sinf(x * 0.093f + t * 2.7f)
              + 2.0f * sinf(x * 0.19f - t * 3.9f);
    return h < 3.0f ? 3.0f : h;
}

static void backdrop_trail_push(backdrop_particle* p) {
    p->trailHead = (u8) ((p->trailHead + 1) % BACKDROP_TRAIL);
    p->trailX[p->trailHead] = p->x;
    p->trailY[p->trailHead] = p->y;
    if(p->trailCount < backdrop_trail_max()) {
        p->trailCount++;
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
    p->vx = 0;
    p->age = 0;
    p->life = 1;
    p->trailCount = 0;
    p->trailHead = 0;
    p->trailTimer = 0;
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
        case BACKDROP_FOREST:
            // Rises from the ground, wanders in loops for a while and fades. At the start some wait a little, so they
            // do not all come up together.
            p->size = (2.0f + 1.2f * backdrop_random()) * (0.7f + 0.3f * near);
            p->speed = (32.0f + 16.0f * backdrop_random()) * (0.6f + 0.4f * near);
            p->x = backdrop_width * (0.08f + 0.84f * backdrop_random());
            p->y = backdrop_height + 6;
            p->angle = -BACKDROP_PI / 2 + (backdrop_random() - 0.5f) * 0.8f;
            p->life = 8.0f + 5.0f * backdrop_random();
            p->age = anywhere ? -backdrop_random() * 4.0f : -backdrop_random() * 2.0f;
            break;
        case BACKDROP_THREADS:
            // A thread grows from the ground and goes up, swaying, thinner and fainter until it is gone.
            p->size = (1.3f + 0.8f * backdrop_random()) * (0.7f + 0.3f * near);
            p->speed = (30.0f + 20.0f * backdrop_random()) * (0.6f + 0.4f * near);
            p->base = backdrop_width * (0.05f + 0.9f * backdrop_random());
            p->x = p->base;
            p->y = backdrop_height + 4;
            p->life = (backdrop_height + 40) / p->speed;
            p->age = anywhere ? -backdrop_random() * 4.0f : -backdrop_random() * 2.5f;
            break;
        case BACKDROP_ENERGY: {
            // A ball of energy where the surface swells most: the highest of a few places tried.
            float best = 0;
            for(u32 k = 0; k < 4; k++) {
                float x = backdrop_random() * backdrop_width;
                float h = backdrop_energy_height(x);
                if(h > best) {
                    best = h;
                    p->base = x;
                }
            }

            p->size = (1.2f + 1.0f * backdrop_random()) * (0.7f + 0.3f * near);
            p->speed = (18.0f + 18.0f * backdrop_random()) * (0.6f + 0.4f * near);
            p->x = p->base;
            p->y = backdrop_height - best;
            p->vx = (backdrop_random() - 0.5f) * 6.0f;
            p->life = 2.0f + 2.0f * backdrop_random();
            p->age = anywhere ? -backdrop_random() * p->life : 0;
            break;
        }
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

bool backdrop_enabled() {
    return backdrop_style_now != BACKDROP_OFF;
}

// Still while a transfer has the turn: nothing is drawn for it then, and the transfer keeps the CPU.
static bool backdrop_paused() {
    xferturn_status status;
    xferturn_get(&status);
    return status.busy;
}

bool backdrop_active() {
    return backdrop_style_now != BACKDROP_OFF && !backdrop_paused();
}

// Records the trail of the styles that have one.
static void backdrop_trail_update(backdrop_particle* p, float dt) {
    p->trailTimer += dt;
    float step = backdrop_trail_step();
    if(p->trailCount == 0 || p->trailTimer >= step) {
        p->trailTimer = p->trailCount == 0 ? 0 : p->trailTimer - step;
        backdrop_trail_push(p);
    }
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
            case BACKDROP_FOREST: {
                p->age += dt;
                if(p->age < 0) {
                    break;
                }

                if(p->age > p->life) {
                    backdrop_spawn(p, false);
                    break;
                }

                // Up fast at first, then slower, in loops: the heading turns more as it slows down, and is drawn back
                // towards up so it does not dive to the ground.
                float t = p->age / p->life;
                float speed = p->speed * (1.0f - 0.6f * t);
                float turn = (0.8f + 2.4f * t) * sinf(backdrop_time * (1.1f + 0.3f * p->far) + p->phase);
                p->angle += (turn + 1.4f * (1.0f - t) * sinf(-BACKDROP_PI / 2 - p->angle)) * dt;
                p->x += cosf(p->angle) * speed * dt;
                p->y += sinf(p->angle) * speed * dt;
                if(p->x < 4 || p->x > backdrop_width - 4) {
                    p->angle = BACKDROP_PI - p->angle;
                }

                backdrop_trail_update(p, dt);
                break;
            }
            case BACKDROP_THREADS: {
                p->age += dt;
                if(p->age < 0) {
                    break;
                }

                p->y -= p->speed * dt;
                // The sway grows as it rises.
                float risen = (backdrop_height - p->y) / backdrop_height;
                p->x = p->base + (6.0f + 20.0f * risen) * sinf(p->y / 30.0f + p->phase + backdrop_time * 0.7f);
                backdrop_trail_update(p, dt);
                if(p->y < -40) {
                    backdrop_spawn(p, false);
                }

                break;
            }
            case BACKDROP_ENERGY: {
                p->age += dt;
                if(p->age < 0) {
                    break;
                }

                if(p->age > p->life) {
                    backdrop_spawn(p, false);
                    break;
                }

                // Up and a little sideways, one way and the other, slowing as it fades.
                float t = p->age / p->life;
                p->vx += 10.0f * sinf(p->y / 16.0f + backdrop_time * 2.3f + p->phase) * dt;
                p->vx *= 1.0f - 0.8f * dt;
                p->x += p->vx * dt;
                p->y -= p->speed * (1.0f - 0.5f * t) * dt;
                break;
            }
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

// Colours as 0x00BBGGRR. The last three styles have their own: light at night, deeper by day, where a light colour
// would not show on the pale background.
#define BACKDROP_RGB(r, g, b) ((u32) (r) | ((u32) (g) << 8) | ((u32) (b) << 16))

// Fading in at the start and out at the end of a life.
static float backdrop_fade(float age, float life, float in, float out) {
    float a = 1.0f;
    if(age < in) {
        a = age / in;
    }

    if(age > life - out) {
        float o = (life - age) / out;
        a = o < a ? o : a;
    }

    return a < 0 ? 0 : a;
}

static void backdrop_draw_forest(backdrop_particle* p, bool night) {
    if(p->age < 0 || p->trailCount == 0) {
        return;
    }

    float near = 1.0f - p->far;
    // Golden lights, some greener.
    bool green = p->phase > 4.2f;
    u32 core = night ? (green ? BACKDROP_RGB(210, 255, 170) : BACKDROP_RGB(255, 236, 150)) : (green ? BACKDROP_RGB(70, 150, 40) : BACKDROP_RGB(190, 130, 20));
    u32 glow = night ? (green ? BACKDROP_RGB(140, 230, 110) : BACKDROP_RGB(255, 200, 90)) : (green ? BACKDROP_RGB(90, 170, 60) : BACKDROP_RGB(210, 160, 50));
    float fade = backdrop_fade(p->age, p->life, 0.8f, 1.8f) * (0.6f + 0.4f * near);
    float flicker = 0.8f + 0.2f * sinf(backdrop_time * 6.0f + p->phase);

    // The tail: from the oldest point, small and faint, to the head, with a dot between each two so it stays a line.
    float prevX = 0;
    float prevY = 0;
    for(u32 k = 0; k < p->trailCount; k++) {
        u32 index = (p->trailHead + BACKDROP_TRAIL - (p->trailCount - 1 - k)) % BACKDROP_TRAIL;
        float along = (float) (k + 1) / (float) p->trailCount;
        float x = p->trailX[index];
        float y = p->trailY[index];
        float radius = p->size * (0.3f + 0.6f * along);
        u32 c = backdrop_color(glow, fade * 0.5f * along * sqrtf(along));
        if(k > 0) {
            screen_fill_circle((x + prevX) / 2, (y + prevY) / 2, radius, c);
        }

        screen_fill_circle(x, y, radius, c);
        prevX = x;
        prevY = y;
    }

    screen_fill_circle(p->x, p->y, p->size * 4.0f, backdrop_color(glow, fade * 0.12f * flicker));
    screen_fill_circle(p->x, p->y, p->size * 2.0f, backdrop_color(glow, fade * 0.30f * flicker));
    screen_fill_circle(p->x, p->y, p->size, backdrop_color(core, fade * 0.95f));
}

static void backdrop_draw_thread(backdrop_particle* p, bool night) {
    if(p->age < 0 || p->trailCount == 0) {
        return;
    }

    float near = 1.0f - p->far;
    u32 color = night ? (p->phase > 3.1f ? BACKDROP_RGB(235, 250, 255) : BACKDROP_RGB(170, 225, 255)) : (p->phase > 3.1f ? BACKDROP_RGB(60, 110, 170) : BACKDROP_RGB(40, 130, 190));
    // Fainter and thinner as it rises; it comes out of the ground fading in.
    float risen = (backdrop_height - p->y) / backdrop_height;
    float fade = (risen < 0.08f ? risen / 0.08f : 1.0f) * (1.0f - risen * 0.7f) * (0.6f + 0.4f * near);
    if(fade <= 0) {
        return;
    }

    // The body: its points, and a dot between each two, so it reads as a line.
    float prevX = 0;
    float prevY = 0;
    for(u32 k = 0; k < p->trailCount; k++) {
        u32 index = (p->trailHead + BACKDROP_TRAIL - (p->trailCount - 1 - k)) % BACKDROP_TRAIL;
        float along = (float) (k + 1) / (float) p->trailCount;
        float x = p->trailX[index];
        float y = p->trailY[index];
        float width = p->size * (0.5f + 0.5f * along) * (1.0f - 0.5f * risen);
        u32 c = backdrop_color(color, fade * (0.25f + 0.7f * along));
        if(k > 0) {
            screen_fill_circle((x + prevX) / 2, (y + prevY) / 2, width, c);
        }

        screen_fill_circle(x, y, width, c);
        prevX = x;
        prevY = y;
    }

    screen_fill_circle(p->x, p->y, p->size * 2.6f, backdrop_color(color, fade * 0.25f));
    screen_fill_circle(p->x, p->y, p->size * 1.2f, backdrop_color(color, fade * 0.9f));
}

// Columns of this width, three quads each: 80 across the screen (core 0 ~30% with 3 px and four quads).
#define BACKDROP_ENERGY_COLUMN 5.0f

static void backdrop_draw_energy_ground(bool night) {
    // The whole bottom glows, in a surface that swells and sinks (backdrop_energy_height): brighter where it is higher,
    // denser towards the ground, with a bright crest.
    u32 glow = night ? BACKDROP_RGB(60, 220, 120) : BACKDROP_RGB(30, 150, 80);
    u32 crest = night ? BACKDROP_RGB(190, 255, 215) : BACKDROP_RGB(30, 160, 80);
    screen_set_depth(-2.0f);
    for(float x = 0; x < backdrop_width; x += BACKDROP_ENERGY_COLUMN) {
        float h = backdrop_energy_height(x + BACKDROP_ENERGY_COLUMN / 2);
        float strength = 0.5f + 0.5f * (h / 41.0f);
        float top = backdrop_height - h;
        // The surface and a denser glow towards the ground.
        screen_fill_rect(x, top, BACKDROP_ENERGY_COLUMN, h, backdrop_color(glow, 0.10f * strength));
        screen_fill_rect(x, backdrop_height - h * 0.55f, BACKDROP_ENERGY_COLUMN, h * 0.55f, backdrop_color(glow, 0.12f * strength));
        screen_fill_rect(x, top - 1, BACKDROP_ENERGY_COLUMN, 2, backdrop_color(crest, 0.35f * strength));
    }
}

static void backdrop_draw_energy(backdrop_particle* p, bool night) {
    if(p->age < 0) {
        return;
    }

    float near = 1.0f - p->far;
    float t = p->age / p->life;
    // A ball with a glow: bright as it leaves the surface, greener and smaller as it fades.
    u32 bright = night ? BACKDROP_RGB(210, 255, 225) : BACKDROP_RGB(40, 170, 90);
    u32 green = night ? BACKDROP_RGB(90, 240, 150) : BACKDROP_RGB(30, 140, 70);
    float fade = backdrop_fade(p->age, p->life, 0.3f, p->life * 0.6f) * (0.6f + 0.4f * near);
    float size = p->size * (1.0f - 0.35f * t);
    screen_fill_circle(p->x, p->y, size * 3.0f, backdrop_color(green, fade * 0.14f));
    screen_fill_circle(p->x, p->y, size * 1.5f, backdrop_color(green, fade * 0.22f));
    screen_fill_circle(p->x, p->y, size, backdrop_color(t < 0.4f ? bright : green, fade * 0.85f));
}

void backdrop_draw(float width, float height) {
    if(backdrop_style_now == BACKDROP_OFF) {
        return;
    }

    backdrop_width = width;
    backdrop_height = height;
    const theme_current* theme_now = theme();
    u32 accent = theme_now->accent;
    bool night = theme_now->night;
    float before = screen_get_depth();
    if(backdrop_style_now == BACKDROP_ENERGY) {
        backdrop_draw_energy_ground(night);
    }

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
            case BACKDROP_FOREST:
                backdrop_draw_forest(p, night);
                break;
            case BACKDROP_THREADS:
                backdrop_draw_thread(p, night);
                break;
            case BACKDROP_ENERGY:
                backdrop_draw_energy(p, night);
                break;
            default:
                break;
        }
    }

    screen_set_depth(before);
}
