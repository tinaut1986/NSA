#include <string.h>

#include <3ds.h>

#include "cia.h"
#include "smdh.h"
#include "tmd.h"
#include "../error.h"

Result cia_get_title_id(u64* titleId, u8* cia, size_t size) {
    if(cia == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    if(size < 0x10) {
        return R_APP_BAD_DATA;
    }

    u32 headerSize = ((*(u32*) &cia[0x00]) + 0x3F) & ~0x3F;
    u32 certSize = ((*(u32*) &cia[0x08]) + 0x3F) & ~0x3F;
    u32 ticketSize = ((*(u32*) &cia[0x0C]) + 0x3F) & ~0x3F;
    u32 offset = headerSize + certSize + ticketSize;

    if(offset >= size) {
        return R_APP_BAD_DATA;
    }

    return tmd_get_title_id(titleId, &cia[offset], size - offset);
}

Result cia_get_title_version(u16* version, u8* cia, size_t size) {
    if(cia == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    if(size < 0x10) {
        return R_APP_BAD_DATA;
    }

    u32 headerSize = ((*(u32*) &cia[0x00]) + 0x3F) & ~0x3F;
    u32 certSize = ((*(u32*) &cia[0x08]) + 0x3F) & ~0x3F;
    u32 ticketSize = ((*(u32*) &cia[0x0C]) + 0x3F) & ~0x3F;
    u32 offset = headerSize + certSize + ticketSize;

    if(offset >= size) {
        return R_APP_BAD_DATA;
    }

    return tmd_get_title_version(version, &cia[offset], size - offset);
}

Result cia_file_get_smdh(SMDH* smdh, Handle handle) {
    Result res = 0;

    if(smdh != NULL) {
        u32 bytesRead = 0;

        u32 header[8];
        if(R_SUCCEEDED(res = FSFILE_Read(handle, &bytesRead, 0, header, sizeof(header))) && bytesRead == sizeof(header)) {
            u32 headerSize = (header[0] + 0x3F) & ~0x3F;
            u32 certSize = (header[2] + 0x3F) & ~0x3F;
            u32 ticketSize = (header[3] + 0x3F) & ~0x3F;
            u32 tmdSize = (header[4] + 0x3F) & ~0x3F;
            u32 metaSize = (header[5] + 0x3F) & ~0x3F;
            u64 contentSize = ((header[6] | ((u64) header[7] << 32)) + 0x3F) & ~0x3F;

            if(metaSize >= 0x3AC0) {
                res = FSFILE_Read(handle, &bytesRead, headerSize + certSize + ticketSize + tmdSize + contentSize + 0x400, smdh, sizeof(SMDH));
            } else {
                res = R_APP_BAD_DATA;
            }
        }
    } else {
        res = R_APP_INVALID_ARGUMENT;
    }

    return res;
}

void cia_seed_watch_start(cia_seed_watch* watch, const u8* cia, size_t size) {
    memset(watch, 0, sizeof(*watch));
    if(cia != NULL && size >= 0x20) {
        u64 headerSize = ((*(u32*) &cia[0x00]) + 0x3F) & ~0x3F;
        u64 certSize = ((*(u32*) &cia[0x08]) + 0x3F) & ~0x3F;
        u64 ticketSize = ((*(u32*) &cia[0x0C]) + 0x3F) & ~0x3F;
        u64 tmdSize = ((*(u32*) &cia[0x10]) + 0x3F) & ~0x3F;
        watch->content = headerSize + certSize + ticketSize + tmdSize;
    }
}

void cia_seed_watch_feed(cia_seed_watch* watch, const void* buffer, u64 offset, u32 size) {
    if(watch->content == 0 || buffer == NULL) {
        return;
    }

    const u8* bytes = (const u8*) buffer;
    for(u32 i = 0; i < 5; i++) {
        u64 at = watch->content + (i < 4 ? 0x100 + i : 0x18F);
        if(at >= offset && at < offset + size) {
            if(i < 4) {
                watch->magic[i] = bytes[at - offset];
            } else {
                watch->flags7 = bytes[at - offset];
            }

            watch->seen |= 1 << i;
        }
    }
}

bool cia_seed_watch_needed(const cia_seed_watch* watch) {
    if(watch->seen != 0x1F || memcmp(watch->magic, "NCCH", 4) != 0) {
        return true;
    }

    return (watch->flags7 & 0x20) != 0;
}
