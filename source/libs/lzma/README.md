# LZMA SDK (7z decoder)

The C files of the 7z decoder from 7-Zip 24.09 (`C/` of https://github.com/ip7z/7zip, tag 24.09), unchanged. The LZMA SDK
is in the public domain. NSA uses it in `source/core/unarchive.c` for the Store's .7z archives; PPMd archives are read too
(`-DZ7_PPMD_SUPPORT` in the Makefile).
