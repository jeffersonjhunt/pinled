#pragma once

/**
 * @file harness.h
 * @brief Minimal assertion harness for the host tests.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Sixty lines instead of a dependency. doctest or Catch2 would both be fine
 * choices, but each is a vendored ~10k-line header or a network fetch at
 * configure time, and neither buys anything at this scale: CTest already
 * supplies discovery, parallelism and reporting, so all that is missing is
 * CHECK.
 *
 * Failures do not abort. A run reports every failing assertion in one pass,
 * which matters when a schema change breaks thirty round-trips and you want
 * the shape of the breakage rather than the first instance of it.
 *
 * Usage:
 * @code
 *   TEST(name_of_thing) { CHECK_EQ(a, b); }
 *   int main() { return ooe::test::run_all(); }
 * @endcode
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ooe::test
{
    struct Case
    {
        const char *name;
        void (*fn)();
    };

    inline std::vector<Case> &registry()
    {
        static std::vector<Case> r;
        return r;
    }

    inline int &failures()
    {
        static int f = 0;
        return f;
    }

    inline const char *&current()
    {
        static const char *c = "";
        return c;
    }

    struct Registrar
    {
        Registrar(const char *name, void (*fn)())
        {
            registry().push_back({name, fn});
        }
    };

    inline void fail(const char *file, int line, const std::string &what)
    {
        std::printf("  FAIL %s:%d\n       %s\n", file, line, what.c_str());
        ++failures();
    }

    /// Render a value for a failure message without requiring operator<<.
    template <typename T>
    inline std::string show(const T &v)
    {
        if constexpr (std::is_same_v<T, bool>)
            return v ? "true" : "false";
        else if constexpr (std::is_enum_v<T>)
            return std::to_string(static_cast<long long>(v));
        else if constexpr (std::is_integral_v<T>)
            return std::to_string(v);
        else
            return "<value>";
    }

    inline std::string show(const char *v) { return v ? std::string("\"") + v + "\"" : "null"; }
    inline std::string show(const std::string &v) { return "\"" + v + "\""; }

    inline int run_all()
    {
        int failed_cases = 0;
        for (const auto &c : registry())
        {
            const int before = failures();
            current() = c.name;
            c.fn();
            const bool ok = failures() == before;
            std::printf("%s %s\n", ok ? "  ok  " : "NOT OK", c.name);
            if (!ok)
                ++failed_cases;
        }
        std::printf("\n%zu cases, %d failed assertion(s) in %d case(s)\n",
                    registry().size(), failures(), failed_cases);
        return failed_cases == 0 ? 0 : 1;
    }
} // namespace ooe::test

#define TEST(name)                                                       \
    static void test_##name();                                           \
    static ::ooe::test::Registrar reg_##name(#name, &test_##name);       \
    static void test_##name()

#define CHECK(expr)                                                      \
    do {                                                                 \
        if (!(expr))                                                     \
            ::ooe::test::fail(__FILE__, __LINE__, "expected: " #expr);   \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        const auto _a = (a);                                             \
        const auto _b = (b);                                             \
        if (!(_a == _b))                                                 \
            ::ooe::test::fail(__FILE__, __LINE__,                        \
                              std::string(#a " == " #b "  (got ") +      \
                                  ::ooe::test::show(_a) + ", want " +    \
                                  ::ooe::test::show(_b) + ")");          \
    } while (0)

#define CHECK_NE(a, b)                                                   \
    do {                                                                 \
        if ((a) == (b))                                                  \
            ::ooe::test::fail(__FILE__, __LINE__, #a " != " #b);         \
    } while (0)
