/*
  PreviousTurbo - misc_tests.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  String and file name helpers.
*/
#include <cstring>

#include "main.h"
#include "file.h"
extern "C" {
#include "str.h"
}
#include "test.h"

TEST(str_trim) {
    char a[] = "  \t hello world \n ";
    CHECK_STR(Str_Trim(a), "hello world");
    char b[] = "    ";
    CHECK_STR(Str_Trim(b), "");
    char c[] = "x";
    CHECK_STR(Str_Trim(c), "x");
}

TEST(str_case) {
    char a[] = "Rev_2.5_v66.BIN";
    CHECK_STR(Str_ToLower(a), "rev_2.5_v66.bin");
    CHECK_STR(Str_ToUpper(a), "REV_2.5_V66.BIN");
}

TEST(str_copy_truncates) {
    char buf[6];
    CHECK_EQ(Str_Copy(buf, "abc", sizeof buf), 3L);
    CHECK_STR(buf, "abc");
    CHECK(Str_Copy(buf, "abcdefgh", sizeof buf) < 0);
    CHECK_STR(buf, "abcde");
}

TEST(str_unescape) {
    char a[] = "a\\tb\\nc\\\\d\\q";
    Str_UnEscape(a);
    CHECK_STR(a, "a\tb\nc\\d\\q");
}

TEST(str_find_in_mem) {
    uint8_t mem[] = {'x', 'N', 'e', 'X', 'T', 'y'};
    CHECK(Str_FindInMem("NeXT", mem, sizeof mem) == mem + 1);
    CHECK(Str_FindInMem("Cube", mem, sizeof mem) == nullptr);
}

TEST(file_extension_match) {
    CHECK(File_DoesFileExtensionMatch("disk.img", ".img"));
    CHECK(File_DoesFileExtensionMatch("ROM.BIN", ".bin"));
    CHECK(!File_DoesFileExtensionMatch("disk.img", ".iso"));
    CHECK(!File_DoesFileExtensionMatch("a", ".img"));
}

TEST(file_split_and_make_path) {
    char dir[FILENAME_MAX], name[FILENAME_MAX], ext[FILENAME_MAX];
    const char sep[2] = {PATHSEP, 0};
    char path[FILENAME_MAX];
    snprintf(path, sizeof path, "disks%snext%sboot.sd", sep, sep);
    File_SplitPath(path, dir, name, ext);
    char want_dir[FILENAME_MAX];
    snprintf(want_dir, sizeof want_dir, "disks%snext", sep);
    CHECK_STR(dir, want_dir);
    CHECK_STR(name, "boot");
    CHECK_STR(ext, "sd");

    char* made = File_MakePath(want_dir, "boot", ".sd");
    CHECK(made != nullptr);
    if (made) {
        CHECK_STR(made, path);
        free(made);
    }
}
