/*
  PreviousTurbo - test_main.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Runs the unit tests: all of them, or those whose name contains the first
  argument.
*/
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" {
#include "paths.h"
}
#include "test.h"

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // A home of their own, so the tests make no Documents/PreviousTurbo in
    // the real one
    const auto home = std::filesystem::temp_directory_path() / "pt_tests_home";
    std::filesystem::create_directories(home);
    setenv("HOME", home.string().c_str(), 1);
#endif
    Paths_Init(argv[0]);
    int run = 0, failed = 0;
    for (const auto& c : pt::test::registry()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        const int before = pt::test::failures();
        c.fn();
        ++run;
        const bool ok = pt::test::failures() == before;
        if (!ok) ++failed;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("%d/%d tests passed\n", run - failed, run);
    return failed ? 1 : 0;
}
