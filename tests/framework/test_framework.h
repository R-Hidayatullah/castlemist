/// @file
/// @brief A ~200-line test framework with no dependencies beyond the standard library.
///
/// castlemist is built with a bare MinGW toolchain and no package manager, so
/// pulling in GoogleTest or Catch2 would mean one more checkout under external/
/// for very little. The needs here are modest -- assert, group, report, filter,
/// skip -- so the framework is local.
///
/// Usage:
/// @code
/// #include "test_framework.h"
/// #include "castlemist/core/bytes.h"
///
/// CM_TEST(bytes, u32_is_little_endian) {
///     const uint8_t raw[] = {0x78, 0x56, 0x34, 0x12};
///     castlemist::core::Bytes b(raw, sizeof raw);
///     CHECK_EQ(b.u32(0), 0x12345678u);
/// }
/// @endcode
///
/// Every test executable links `test_main.cpp`, which runs the registry and
/// returns a non-zero exit code on the first failing suite. Pass a substring on
/// the command line to run only matching tests.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace castlemist::test {

/// @brief Thrown by the CHECK_* macros; carries the already-formatted message.
struct Failure {
    std::string message;
};

/// @brief Thrown by SKIP(); reports the test as skipped rather than failed.
struct Skipped {
    std::string reason;
};

/// @brief One registered test case.
struct TestCase {
    const char* group;
    const char* name;
    void (*fn)();
};

/// @brief The process-wide registry. Defined in test_main.cpp.
std::vector<TestCase>& registry();

/// @brief Static-initialiser helper used by CM_TEST.
struct Registrar {
    Registrar(const char* group, const char* name, void (*fn)()) {
        registry().push_back(TestCase{group, name, fn});
    }
};

/// @brief Run every registered test whose "group.name" contains @p filter.
/// @return Process exit code: 0 when nothing failed.
int run_all(const char* filter);

// ---------------------------------------------------------------- formatting

inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return v ? std::string("\"") + v + "\"" : "(null)"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(std::nullptr_t) { return "nullptr"; }

inline std::string show(const std::wstring& v) {
    // Tests only ever compare ASCII-ish wide strings; escape the rest as \xNNNN
    // so a failure message stays printable on a code-page-935 console.
    std::string s = "L\"";
    for (wchar_t c : v) {
        if (c >= 0x20 && c < 0x7F) {
            s.push_back(static_cast<char>(c));
        } else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\x%04X", static_cast<unsigned>(c));
            s += buf;
        }
    }
    return s + "\"";
}
inline std::string show(const wchar_t* v) { return v ? show(std::wstring(v)) : "(null)"; }

/// @brief Byte buffers print as a length plus a short hex head, never in full:
///        a mismatched decode is megabytes long and the first difference is
///        almost always in the first few bytes.
inline std::string show(const std::vector<unsigned char>& v) {
    std::string s = std::to_string(v.size()) + " bytes [";
    const size_t shown = v.size() < 12 ? v.size() : 12;
    for (size_t i = 0; i < shown; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof buf, i ? " %02X" : "%02X", v[i]);
        s += buf;
    }
    return s + (shown < v.size() ? " ...]" : "]");
}

template <typename T>
std::string show(const T& v) {
    return std::to_string(v);
}

/// @brief Raise a Failure with file/line context.
[[noreturn]] inline void fail(const char* file, int line, const std::string& what) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + "\n      " + what};
}

} // namespace castlemist::test

// --------------------------------------------------------------------- macros

/// @brief Define and register a test case.
/// @param group Suite name (usually the module under test).
/// @param name  Test name; must be a valid identifier.
#define CM_TEST(group, name)                                      \
    static void cm_test_##group##_##name();                       \
    static ::castlemist::test::Registrar cm_reg_##group##_##name( \
        #group, #name, &cm_test_##group##_##name);                \
    static void cm_test_##group##_##name()

/// @brief Assert that @p expr is true.
#define CHECK(expr)                                                                             \
    do {                                                                                        \
        if (!(expr)) ::castlemist::test::fail(__FILE__, __LINE__, "CHECK(" #expr ") is false"); \
    } while (0)

/// @brief Assert that @p expr is false.
#define CHECK_FALSE(expr)                                                                         \
    do {                                                                                          \
        if (expr) ::castlemist::test::fail(__FILE__, __LINE__, "CHECK_FALSE(" #expr ") is true"); \
    } while (0)

/// @brief Assert `a == b`, printing both values on failure.
#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const auto& _a = (a);                                                       \
        const auto& _b = (b);                                                       \
        if (!(_a == _b))                                                            \
            ::castlemist::test::fail(__FILE__, __LINE__,                            \
                            std::string(#a " == " #b "\n      left  = ") +          \
                                ::castlemist::test::show(_a) + "\n      right = " + \
                                ::castlemist::test::show(_b));                      \
    } while (0)

/// @brief Assert `a != b`.
#define CHECK_NE(a, b)                                                   \
    do {                                                                 \
        const auto& _a = (a);                                            \
        const auto& _b = (b);                                            \
        if (_a == _b)                                                    \
            ::castlemist::test::fail(__FILE__, __LINE__,                 \
                            std::string(#a " != " #b " but both are ") + \
                                ::castlemist::test::show(_a));           \
    } while (0)

/// @brief Assert `|a - b| <= tol` for floating-point values.
#define CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                            \
        double _a = static_cast<double>(a);                                         \
        double _b = static_cast<double>(b);                                         \
        double _t = static_cast<double>(tol);                                       \
        double _d = _a > _b ? _a - _b : _b - _a;                                    \
        if (!(_d <= _t))                                                            \
            ::castlemist::test::fail(__FILE__, __LINE__,                            \
                            std::string(#a " ~= " #b "\n      left  = ") +          \
                                ::castlemist::test::show(_a) + "\n      right = " + \
                                ::castlemist::test::show(_b) + "\n      delta = " + \
                                ::castlemist::test::show(_d));                      \
    } while (0)

/// @brief Abandon the current test, reporting it as skipped (missing fixture, no dat, ...).
#define SKIP(reason) \
    throw ::castlemist::test::Skipped { reason }
