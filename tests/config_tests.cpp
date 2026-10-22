/*
  PreviousTurbo - config_tests.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Settings files: what is saved is what is loaded again.
*/
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "main.h"
#include "configuration.h"
extern "C" {
#include "paths.h"
#include "str.h"
}
#include "test.h"

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void save_to(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    Str_Copy(sConfigFileName, path.string().c_str(), FILENAME_MAX);
    Configuration_Save();
}

}  // namespace

TEST(config_save_load_round_trip) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto first = dir / "pt_tests_first.cfg";
    const auto second = dir / "pt_tests_second.cfg";

    Configuration_SetDefault();
    ConfigureParams.System.nMachineType = NEXT_STATION;
    ConfigureParams.System.bColor = true;
    ConfigureParams.System.bTurbo = true;
    ConfigureParams.System.nCpuFreq = 33;
    ConfigureParams.Memory.nMemoryBankSize[0] = 16;
    ConfigureParams.Mouse.fLinScale = 2.5f;
    ConfigureParams.Mouse.bEnableMapToKey = !ConfigureParams.Mouse.bEnableMapToKey;
    Str_Copy(ConfigureParams.Rom.szRom040FileName, "/roms/Rev_3.3_v74.BIN", FILENAME_MAX);
    save_to(first);
    CHECK(std::filesystem::exists(first));

    const CNF_PARAMS saved = ConfigureParams;
    Configuration_SetDefault();
    Configuration_Load(first.string().c_str());
    CHECK_EQ(ConfigureParams.System.nMachineType, saved.System.nMachineType);
    CHECK_EQ(ConfigureParams.System.bColor, true);
    CHECK_EQ(ConfigureParams.System.bTurbo, true);
    CHECK_EQ(ConfigureParams.System.nCpuFreq, 33);
    CHECK_EQ(ConfigureParams.Memory.nMemoryBankSize[0], 16);
    CHECK(ConfigureParams.Mouse.fLinScale == 2.5f);
    CHECK_EQ(ConfigureParams.Mouse.bEnableMapToKey, saved.Mouse.bEnableMapToKey);
    CHECK_STR(ConfigureParams.Rom.szRom040FileName, "/roms/Rev_3.3_v74.BIN");

    // Every setting: saving what was loaded gives the same file.
    save_to(second);
    CHECK(read_file(first) == read_file(second));

    std::filesystem::remove(first);
    std::filesystem::remove(second);
    Configuration_SetDefault();
}

TEST(config_in_documents_folder) {
    // Settings in Documents/PreviousTurbo/settings.cfg, disk images in its
    // diskImages folder, made when the program starts
    CHECK(Paths_GetUserData() != nullptr);
    CHECK(Paths_GetDiskImagesDir() != nullptr);
    if (!Paths_GetUserData() || !Paths_GetDiskImagesDir()) return;
    const std::filesystem::path data(Paths_GetUserData());
    CHECK(data.filename() == "PreviousTurbo");
    CHECK(data.parent_path().filename() == "Documents");
    CHECK(std::filesystem::is_directory(data));
    CHECK(std::filesystem::path(Paths_GetDiskImagesDir()) == data / "diskImages");
    CHECK(std::filesystem::is_directory(Paths_GetDiskImagesDir()));

    Configuration_SetDefault();
    CHECK(std::filesystem::path(sConfigFileName) == data / "settings.cfg");
}
