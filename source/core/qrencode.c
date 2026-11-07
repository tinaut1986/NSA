#include <stdlib.h>
#include <string.h>

#include "qrencode.h"

// See qrencode.h. Coordinates: x is the column, y the row.

// Error correction level M, indexed by version (index 0 unused).
static const int8_t qr_ecc_per_block[QR_MAX_VERSION + 1] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const int8_t qr_blocks[QR_MAX_VERSION + 1] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
#define QR_FORMAT_ECC_M 0

#define QR_MAX_CODEWORDS 346   // Raw codewords of version 10.

typedef struct {
    int size;
    uint8_t* dark;
    uint8_t* function;
} qr_grid;

static bool qr_get(const qr_grid* g, int x, int y) {
    return g->dark[y * g->size + x] != 0;
}

static void qr_set_function(qr_grid* g, int x, int y, bool dark) {
    g->dark[y * g->size + x] = dark ? 1 : 0;
    g->function[y * g->size + x] = 1;
}

// ---- sizes ----

// Modules left for data and error correction once the function patterns are drawn.
static int qr_raw_data_modules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if(ver >= 2) {
        int align = ver / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if(ver >= 7) {
            result -= 36;
        }
    }

    return result;
}

static int qr_data_codewords(int ver) {
    return qr_raw_data_modules(ver) / 8 - qr_ecc_per_block[ver] * qr_blocks[ver];
}

static int qr_alignment_positions(int ver, uint8_t out[7]) {
    if(ver == 1) {
        return 0;
    }

    int count = ver / 7 + 2;
    int step = (ver * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
    for(int i = count - 1, pos = ver * 4 + 10; i >= 1; i--, pos -= step) {
        out[i] = (uint8_t) pos;
    }

    out[0] = 6;
    return count;
}

// ---- Reed-Solomon over GF(2^8), polynomial 0x11D ----

static uint8_t qr_gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for(int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }

    return (uint8_t) z;
}

static void qr_rs_divisor(int degree, uint8_t* result) {
    memset(result, 0, (size_t) degree);
    result[degree - 1] = 1;

    uint8_t root = 1;
    for(int i = 0; i < degree; i++) {
        for(int j = 0; j < degree; j++) {
            result[j] = qr_gf_mul(result[j], root);
            if(j + 1 < degree) {
                result[j] ^= result[j + 1];
            }
        }

        root = qr_gf_mul(root, 0x02);
    }
}

static void qr_rs_remainder(const uint8_t* data, int len, const uint8_t* divisor, int degree, uint8_t* result) {
    memset(result, 0, (size_t) degree);
    for(int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ result[0];
        memmove(result, result + 1, (size_t) (degree - 1));
        result[degree - 1] = 0;
        for(int j = 0; j < degree; j++) {
            result[j] ^= qr_gf_mul(divisor[j], factor);
        }
    }
}

// Splits the data into blocks, adds each one's error correction and interleaves them.
static void qr_add_ecc(int ver, const uint8_t* data, uint8_t* out) {
    int blocks = qr_blocks[ver];
    int eccLen = qr_ecc_per_block[ver];
    int raw = qr_raw_data_modules(ver) / 8;
    int dataLen = qr_data_codewords(ver);
    int shortBlocks = blocks - raw % blocks;
    int shortDataLen = raw / blocks - eccLen;

    uint8_t divisor[30];
    qr_rs_divisor(eccLen, divisor);

    uint8_t ecc[30];
    for(int i = 0, k = 0; i < blocks; i++) {
        int len = shortDataLen + (i < shortBlocks ? 0 : 1);
        qr_rs_remainder(data + k, len, divisor, eccLen, ecc);
        for(int j = 0, l = i; j < len; j++, l += blocks) {
            if(j == shortDataLen) {
                l -= shortBlocks;
            }

            out[l] = data[k + j];
        }

        for(int j = 0, l = dataLen + i; j < eccLen; j++, l += blocks) {
            out[l] = ecc[j];
        }

        k += len;
    }
}

// ---- function patterns ----

static void qr_draw_format(qr_grid* g, int mask) {
    int data = QR_FORMAT_ECC_M << 3 | mask;
    int rem = data;
    for(int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }

    int bits = (data << 10 | rem) ^ 0x5412;
    int size = g->size;

    for(int i = 0; i <= 5; i++) {
        qr_set_function(g, 8, i, (bits >> i) & 1);
    }

    qr_set_function(g, 8, 7, (bits >> 6) & 1);
    qr_set_function(g, 8, 8, (bits >> 7) & 1);
    qr_set_function(g, 7, 8, (bits >> 8) & 1);
    for(int i = 9; i < 15; i++) {
        qr_set_function(g, 14 - i, 8, (bits >> i) & 1);
    }

    for(int i = 0; i < 8; i++) {
        qr_set_function(g, size - 1 - i, 8, (bits >> i) & 1);
    }

    for(int i = 8; i < 15; i++) {
        qr_set_function(g, 8, size - 15 + i, (bits >> i) & 1);
    }

    qr_set_function(g, 8, size - 8, true);
}

static void qr_draw_version(qr_grid* g, int ver) {
    if(ver < 7) {
        return;
    }

    int rem = ver;
    for(int i = 0; i < 12; i++) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    }

    long bits = (long) ver << 12 | rem;
    for(int i = 0; i < 18; i++) {
        bool bit = (bits >> i) & 1;
        int a = g->size - 11 + i % 3;
        int b = i / 3;
        qr_set_function(g, a, b, bit);
        qr_set_function(g, b, a, bit);
    }
}

static void qr_draw_finder(qr_grid* g, int x, int y) {
    for(int dy = -4; dy <= 4; dy++) {
        for(int dx = -4; dx <= 4; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            int xx = x + dx;
            int yy = y + dy;
            if(xx >= 0 && xx < g->size && yy >= 0 && yy < g->size) {
                qr_set_function(g, xx, yy, dist != 2 && dist != 4);
            }
        }
    }
}

static void qr_draw_alignment(qr_grid* g, int x, int y) {
    for(int dy = -2; dy <= 2; dy++) {
        for(int dx = -2; dx <= 2; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            qr_set_function(g, x + dx, y + dy, dist != 1);
        }
    }
}

static void qr_draw_functions(qr_grid* g, int ver) {
    int size = g->size;
    for(int i = 0; i < size; i++) {
        qr_set_function(g, 6, i, i % 2 == 0);
        qr_set_function(g, i, 6, i % 2 == 0);
    }

    qr_draw_finder(g, 3, 3);
    qr_draw_finder(g, size - 4, 3);
    qr_draw_finder(g, 3, size - 4);

    uint8_t pos[7];
    int count = qr_alignment_positions(ver, pos);
    for(int i = 0; i < count; i++) {
        for(int j = 0; j < count; j++) {
            if(!((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0))) {
                qr_draw_alignment(g, pos[i], pos[j]);
            }
        }
    }

    // Reserved now (its real bits once the mask is chosen).
    qr_draw_format(g, 0);
    qr_draw_version(g, ver);
}

// ---- data and masks ----

static void qr_draw_codewords(qr_grid* g, const uint8_t* data, int len) {
    int size = g->size;
    int i = 0;
    for(int right = size - 1; right >= 1; right -= 2) {
        if(right == 6) {
            right = 5;
        }

        for(int vert = 0; vert < size; vert++) {
            for(int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if(!g->function[y * size + x] && i < len * 8) {
                    g->dark[y * size + x] = (data[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
            }
        }
    }
}

static void qr_apply_mask(qr_grid* g, int mask) {
    int size = g->size;
    for(int y = 0; y < size; y++) {
        for(int x = 0; x < size; x++) {
            bool invert;
            switch(mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }

            if(invert && !g->function[y * size + x]) {
                g->dark[y * size + x] ^= 1;
            }
        }
    }
}

// The specification's penalty: runs of five or more, 2x2 blocks, patterns like a finder's, and the dark/light balance.
static long qr_penalty(const qr_grid* g) {
    int size = g->size;
    long result = 0;

    for(int pass = 0; pass < 2; pass++) {
        for(int a = 0; a < size; a++) {
            int run = 0;
            bool last = false;
            for(int b = 0; b < size; b++) {
                bool dark = pass == 0 ? qr_get(g, b, a) : qr_get(g, a, b);
                if(b > 0 && dark == last) {
                    run++;
                    if(run == 5) {
                        result += 3;
                    } else if(run > 5) {
                        result++;
                    }
                } else {
                    run = 1;
                    last = dark;
                }
            }

            // 1:1:3:1:1 with four light modules on a side.
            for(int b = 0; b + 10 < size; b++) {
                bool line[11];
                for(int k = 0; k < 11; k++) {
                    line[k] = pass == 0 ? qr_get(g, b + k, a) : qr_get(g, a, b + k);
                }

                static const bool p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
                static const bool p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                if(memcmp(line, p1, sizeof(line)) == 0 || memcmp(line, p2, sizeof(line)) == 0) {
                    result += 40;
                }
            }
        }
    }

    for(int y = 0; y < size - 1; y++) {
        for(int x = 0; x < size - 1; x++) {
            bool c = qr_get(g, x, y);
            if(c == qr_get(g, x + 1, y) && c == qr_get(g, x, y + 1) && c == qr_get(g, x + 1, y + 1)) {
                result += 3;
            }
        }
    }

    int dark = 0;
    for(int i = 0; i < size * size; i++) {
        dark += g->dark[i];
    }

    int total = size * size;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += (k > 0 ? k : 0) * 10;
    return result;
}

bool qr_encode(const char* text, qr_code* out) {
    int len = (int) strlen(text);

    int ver = 1;
    for(; ver <= QR_MAX_VERSION; ver++) {
        int countBits = ver <= 9 ? 8 : 16;
        if(4 + countBits + len * 8 <= qr_data_codewords(ver) * 8) {
            break;
        }
    }

    if(ver > QR_MAX_VERSION) {
        return false;
    }

    // Byte mode: 0100, the length, the bytes, a terminator, then the padding bytes.
    uint8_t data[QR_MAX_CODEWORDS];
    memset(data, 0, sizeof(data));
    int capacity = qr_data_codewords(ver) * 8;
    int bit = 0;
#define QR_PUT(value, count) do { for(int i_ = (count) - 1; i_ >= 0; i_--, bit++) { data[bit >> 3] |= (((value) >> i_) & 1) << (7 - (bit & 7)); } } while(0)
    QR_PUT(4, 4);
    QR_PUT(len, ver <= 9 ? 8 : 16);
    for(int i = 0; i < len; i++) {
        QR_PUT((uint8_t) text[i], 8);
    }

    int terminator = capacity - bit < 4 ? capacity - bit : 4;
    QR_PUT(0, terminator);
    bit = (bit + 7) / 8 * 8;
    for(uint8_t pad = 0xEC; bit < capacity; pad ^= 0xEC ^ 0x11) {
        QR_PUT(pad, 8);
    }
#undef QR_PUT

    uint8_t codewords[QR_MAX_CODEWORDS];
    qr_add_ecc(ver, data, codewords);

    int size = ver * 4 + 17;
    uint8_t function[QR_MAX_SIZE * QR_MAX_SIZE];
    memset(function, 0, sizeof(function));
    memset(out->modules, 0, sizeof(out->modules));
    out->size = size;

    qr_grid g = {size, out->modules, function};
    qr_draw_functions(&g, ver);
    qr_draw_codewords(&g, codewords, qr_raw_data_modules(ver) / 8);

    int best = 0;
    long bestPenalty = -1;
    for(int mask = 0; mask < 8; mask++) {
        qr_apply_mask(&g, mask);
        qr_draw_format(&g, mask);
        long penalty = qr_penalty(&g);
        if(bestPenalty < 0 || penalty < bestPenalty) {
            best = mask;
            bestPenalty = penalty;
        }

        qr_apply_mask(&g, mask);
    }

    qr_apply_mask(&g, best);
    qr_draw_format(&g, best);
    return true;
}
