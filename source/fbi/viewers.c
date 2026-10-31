#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "viewers.h"
#include "../core/core.h"

static bool viewers_ext_in(const char* ext, const char* list) {
    size_t len = strlen(ext);
    const char* p = list;
    while(*p != '\0') {
        const char* end = strchr(p, ' ');
        size_t n = end != NULL ? (size_t) (end - p) : strlen(p);
        if(n == len && strncasecmp(p, ext, n) == 0) {
            return true;
        }

        p += n;
        if(*p == ' ') {
            p++;
        }
    }

    return false;
}

viewer_kind viewers_kind_of(const char* name) {
    const char* dot = strrchr(name, '.');
    if(dot == NULL || dot == name) {
        return viewers_ext_in(name, "makefile readme license") ? VIEWER_TEXT : VIEWER_NONE;
    }

    const char* ext = dot + 1;
    if(viewers_ext_in(ext, "png jpg jpeg bmp gif tga mpo")) {
        return VIEWER_IMAGE;
    }

    if(viewers_ext_in(ext, "mp3 wav flac ogg oga")) {
        return VIEWER_AUDIO;
    }

    if(viewers_ext_in(ext, "txt md json ini cfg conf log xml csv tsv html htm css js c h cpp hpp py lua sh yml yaml toml srt bat cmake properties inf cue m3u tex")) {
        return VIEWER_TEXT;
    }

    return VIEWER_NONE;
}

Result viewers_read_file(FS_Archive archive, const char* path, u32 maxSize, u8** data, u32* size, u64* fullSize) {
    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle handle = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenFile(&handle, archive, *fsPath, FS_OPEN_READ, 0))) {
        u64 total = 0;
        if(R_SUCCEEDED(res = FSFILE_GetSize(handle, &total))) {
            u32 want = total < maxSize ? (u32) total : maxSize;
            u8* buffer = (u8*) malloc((size_t) want + 1);
            if(buffer != NULL) {
                u32 done = 0;
                while(done < want && R_SUCCEEDED(res)) {
                    u32 got = 0;
                    res = FSFILE_Read(handle, &got, done, buffer + done, want - done);
                    if(R_SUCCEEDED(res) && got == 0) {
                        break;
                    }

                    done += got;
                }

                if(R_SUCCEEDED(res)) {
                    buffer[done] = '\0';
                    *data = buffer;
                    *size = done;
                    if(fullSize != NULL) {
                        *fullSize = total;
                    }
                } else {
                    free(buffer);
                }
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        FSFILE_Close(handle);
    }

    fs_free_path_utf8(fsPath);
    return res;
}
