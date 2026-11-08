#pragma once

typedef struct SMDH_s SMDH;

Result cia_get_title_id(u64* titleId, u8* cia, size_t size);
Result cia_get_title_version(u16* version, u8* cia, size_t size);
Result cia_file_get_smdh(SMDH* smdh, Handle handle);

// Whether a CIA's title needs a seed, told from its first content's NCCH header as the CIA goes by in pieces (an install
// writes it in order). Only a title whose header says so needs the seed from Nintendo's server; a CIA whose content is
// encrypted (its header unreadable) or that never showed the header is taken as maybe needing one, as FBI always did.
typedef struct {
    u64 content;     // Where the first content starts in the CIA (0: unknown).
    u8 magic[4];     // "NCCH", at content + 0x100.
    u8 flags7;       // NCCH flags[7], at content + 0x18F: 0x20 = uses a seed.
    u8 seen;         // Bytes of the above seen (5: all).
} cia_seed_watch;

void cia_seed_watch_start(cia_seed_watch* watch, const u8* cia, size_t size);
void cia_seed_watch_feed(cia_seed_watch* watch, const void* buffer, u64 offset, u32 size);
bool cia_seed_watch_needed(const cia_seed_watch* watch);