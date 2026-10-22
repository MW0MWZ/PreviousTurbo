/*
  PreviousTurbo - test.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  A tiny dependency-free test harness.
*/
#pragma once

#include <cstdio>
#include <cstring>
#include <vector>

namespace pt::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace pt::test

#define TEST(name)                                                        \
    static void name();                                                   \
    static const ::pt::test::Registrar name##_registrar(#name, name);     \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++::pt::test::failures();                                                 \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        const auto va_ = (a);                                                                   \
        const auto vb_ = (b);                                                                   \
        if (!(va_ == vb_)) {                                                                    \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: 0x%llx != 0x%llx\n", __FILE__, \
                         __LINE__, #a, #b, static_cast<unsigned long long>(va_),                \
                         static_cast<unsigned long long>(vb_));                                 \
            ++::pt::test::failures();                                                           \
        }                                                                                       \
    } while (0)

#define CHECK_STR(a, b)                                                                         \
    do {                                                                                        \
        const char* va_ = (a);                                                                  \
        const char* vb_ = (b);                                                                  \
        if (std::strcmp(va_, vb_) != 0) {                                                       \
            std::fprintf(stderr, "  %s:%d: CHECK_STR(%s, %s) failed: \"%s\" != \"%s\"\n",       \
                         __FILE__, __LINE__, #a, #b, va_, vb_);                                 \
            ++::pt::test::failures();                                                           \
        }                                                                                       \
    } while (0)
