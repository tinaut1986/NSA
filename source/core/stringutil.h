#pragma once

bool string_is_empty(const char* str);
void string_copy(char* dst, const char* src, size_t size);

// "Game Name.cia" from "Game%20Name.cia": %XX escapes turned back into characters, to show an address. It is only for display:
// the address itself keeps its escapes. `in` and `out` may be the same buffer.
void string_url_decode(char* out, const char* in, size_t size);
void string_get_file_name(char* out, const char* file, u32 size);
void string_escape_file_name(char* out, const char* file, size_t size);
void string_get_path_file(char* out, const char* path, u32 size);
void string_get_parent_path(char* out, const char* path, u32 size);