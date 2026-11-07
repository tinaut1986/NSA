#pragma once

#include <stdbool.h>
#include <stdint.h>

// A small QR code encoder: text in byte mode, error correction level M, versions 1 to 10 (up to 213 bytes), the mask with
// the lowest penalty. Written for NSA after the QR code specification (ISO/IEC 18004) and the structure of Project
// Nayuki's QR Code generator; tests/qrencode_test.c checks it by decoding its codes with quirc.

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (QR_MAX_VERSION * 4 + 17)

typedef struct {
    int size;                                   // Modules per side (21 for version 1, 4 more each version).
    uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]; // 1 dark, 0 light, row by row.
} qr_code;

// False when the text does not fit in version 10.
bool qr_encode(const char* text, qr_code* out);

static inline bool qr_dark(const qr_code* qr, int x, int y) {
    return qr->modules[y * qr->size + x] != 0;
}
