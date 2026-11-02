// The list of a folder sent to another console: building, reading back, finding the file
// of a payload byte, and refusing damaged or unsafe lists.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/fbi/foldermanifest.h"

static int failures = 0;

#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while(0)

static void test_paths(void) {
    CHECK(folder_path_ok("a"));
    CHECK(folder_path_ok("a/b/c.txt"));
    CHECK(folder_path_ok("Box 1 (copy).bin"));
    CHECK(!folder_path_ok(""));
    CHECK(!folder_path_ok("/a"));
    CHECK(!folder_path_ok("a//b"));
    CHECK(!folder_path_ok("a/"));
    CHECK(!folder_path_ok("."));
    CHECK(!folder_path_ok("a/./b"));
    CHECK(!folder_path_ok(".."));
    CHECK(!folder_path_ok("a/../b"));
    CHECK(!folder_path_ok("../a"));
    CHECK(!folder_path_ok("a\\b"));
    CHECK(!folder_path_ok("sdmc:/a"));
    CHECK(!folder_path_ok("a?b"));
    CHECK(folder_path_ok("..a"));
    CHECK(folder_path_ok("a.."));
}

static void make(folder_layout* layout) {
    memset(layout, 0, sizeof(*layout));
    CHECK(folder_layout_add(layout, "boxes", true, 0));
    CHECK(folder_layout_add(layout, "boxes/b1.bin", false, 1000));
    CHECK(folder_layout_add(layout, "empty.txt", false, 0));
    CHECK(folder_layout_add(layout, "boxes/b2.bin", false, 500));
    CHECK(folder_layout_add(layout, "sub", true, 0));
    CHECK(folder_layout_add(layout, "sub/z", false, 1));
    CHECK(folder_layout_finish(layout));
}

static void test_roundtrip(void) {
    folder_layout layout;
    make(&layout);

    CHECK(layout.count == 6);
    CHECK(layout.fileCount == 4);
    CHECK(layout.total == layout.head + 1501);

    uint32_t size = 0;
    uint8_t* bytes = folder_manifest_build(&layout, &size);
    CHECK(bytes != NULL);
    CHECK(size == layout.head);

    folder_layout back;
    CHECK(folder_manifest_parse(bytes, size, &back));
    CHECK(back.count == layout.count);
    CHECK(back.total == layout.total);
    for(uint32_t i = 0; i < back.count; i++) {
        CHECK(strcmp(back.entries[i].path, layout.entries[i].path) == 0);
        CHECK(back.entries[i].size == layout.entries[i].size);
        CHECK(back.entries[i].isDir == layout.entries[i].isDir);
        CHECK(back.entries[i].start == layout.entries[i].start);
    }

    // Each byte of the payload belongs to the file that holds it.
    CHECK(folder_layout_find(&back, 0) == NULL);
    CHECK(folder_layout_find(&back, back.head - 1) == NULL);
    CHECK(folder_layout_find(&back, back.head) == &back.entries[1]);
    CHECK(folder_layout_find(&back, back.head + 999) == &back.entries[1]);
    CHECK(folder_layout_find(&back, back.head + 1000) == &back.entries[3]);   // The empty file is skipped.
    CHECK(folder_layout_find(&back, back.head + 1499) == &back.entries[3]);
    CHECK(folder_layout_find(&back, back.head + 1500) == &back.entries[5]);
    CHECK(folder_layout_find(&back, back.total) == NULL);

    // Every shorter or longer cut is refused.
    for(uint32_t cut = 0; cut < size; cut++) {
        folder_layout cutLayout;
        CHECK(!folder_manifest_parse(bytes, cut, &cutLayout));
    }

    uint8_t* longer = (uint8_t*) malloc(size + 1);
    memcpy(longer, bytes, size);
    longer[size] = 0;
    folder_layout extra;
    CHECK(!folder_manifest_parse(longer, size + 1, &extra));
    free(longer);

    folder_layout_free(&back);
    folder_layout_free(&layout);
    free(bytes);
}

static void test_empty(void) {
    folder_layout layout;
    memset(&layout, 0, sizeof(layout));
    CHECK(folder_layout_finish(&layout));
    CHECK(layout.head == 8 && layout.total == 8 && layout.fileCount == 0);

    uint32_t size = 0;
    uint8_t* bytes = folder_manifest_build(&layout, &size);
    folder_layout back;
    CHECK(folder_manifest_parse(bytes, size, &back));
    CHECK(back.count == 0 && folder_layout_find(&back, 8) == NULL);

    folder_layout_free(&back);
    folder_layout_free(&layout);
    free(bytes);
}

static void test_unsafe(void) {
    // A list made by hand with a path that escapes, as a bad sender could send.
    static const uint8_t bad[] = {
        'N', 'S', 'A', 'F', 0, 0, 0, 1,
        1, 0, 5, '.', '.', '/', 'x', 'y', 0, 0, 0, 0, 0, 0, 0, 1
    };
    folder_layout layout;
    CHECK(!folder_manifest_parse(bad, sizeof(bad), &layout));

    static const uint8_t folderWithSize[] = {
        'N', 'S', 'A', 'F', 0, 0, 0, 1,
        0, 0, 1, 'a', 0, 0, 0, 0, 0, 0, 0, 7
    };
    CHECK(!folder_manifest_parse(folderWithSize, sizeof(folderWithSize), &layout));

    static const uint8_t magic[] = {'N', 'S', 'A', 'X', 0, 0, 0, 0};
    CHECK(!folder_manifest_parse(magic, sizeof(magic), &layout));

    static const uint8_t manyEntries[] = {'N', 'S', 'A', 'F', 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!folder_manifest_parse(manyEntries, sizeof(manyEntries), &layout));

    // Sizes that add up past 2^64.
    static const uint8_t huge[] = {
        'N', 'S', 'A', 'F', 0, 0, 0, 2,
        1, 0, 1, 'a', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0,
        1, 0, 1, 'b', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30
    };
    CHECK(!folder_manifest_parse(huge, sizeof(huge), &layout));
}

int main(void) {
    test_paths();
    test_roundtrip();
    test_empty();
    test_unsafe();

    if(failures != 0) {
        printf("foldermanifest_test: %d failures\n", failures);
        return 1;
    }

    printf("foldermanifest_test: ok\n");
    return 0;
}
