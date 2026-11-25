// Minimal self-contained test harness (no third-party frameworks).
//
//   DP_TEST(name) { CHECK(...); CHECK_EQ(a, b); REQUIRE(...); }
//   DP_TEST_MAIN()
//
// Each executable runs all of its cases (or those whose name contains argv[1]) and exits
// non-zero if any check failed, which is what CTest keys on.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dptest {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

struct State {
    long checks = 0;
    long failures = 0;
    long failuresInCase = 0;
};

inline State& state() {
    static State s;
    return s;
}

/// True on a continuous-integration runner (GitHub Actions and most others set CI).
/// Hosted macOS runners are shared virtual machines, so wall-clock and frame-rate
/// expectations are only enforced on real hardware; everything functional still is.
inline bool onCI() {
    const char* value = std::getenv("CI");
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0;
}

/// Thrown by REQUIRE to abandon the current case.
struct AbortCase {};

inline void fail(const char* file, int line, const std::string& message) {
    ++state().failures;
    ++state().failuresInCase;
    if (state().failuresInCase <= 20) {
        std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, message.c_str());
    } else if (state().failuresInCase == 21) {
        std::fprintf(stderr, "    (further failures in this case suppressed)\n");
    }
}

template <typename T>
std::string describe(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, bool>) {
        out << (value ? "true" : "false");
    } else if constexpr (std::is_enum_v<T>) {
        out << static_cast<long long>(value);
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        out << static_cast<long long>(value);
    } else if constexpr (std::is_integral_v<T>) {
        out << static_cast<unsigned long long>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        out.precision(12);
        out << value;
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        out << '"' << std::string_view(value) << '"';
    } else {
        out << "<value>";
    }
    return out.str();
}

template <typename A, typename B>
void checkEqual(const A& a, const B& b, const char* exprA, const char* exprB, const char* file, int line) {
    ++state().checks;
    if (!(a == b)) {
        fail(file, line, std::string("CHECK_EQ(") + exprA + ", " + exprB + ")  left = " + describe(a) + ", right = " + describe(b));
    }
}

inline void checkNear(double a, double b, double tolerance, const char* exprA, const char* exprB, const char* file,
                      int line) {
    ++state().checks;
    if (!(std::fabs(a - b) <= tolerance) || std::isnan(a) || std::isnan(b)) {
        fail(file, line,
             std::string("CHECK_NEAR(") + exprA + ", " + exprB + ")  left = " + describe(a) + ", right = " + describe(b) +
                 ", tolerance = " + describe(tolerance));
    }
}

inline int runAll(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (filter != nullptr && std::strcmp(filter, "--list") == 0) {
        for (const TestCase& c : registry()) std::printf("%s\n", c.name);
        return 0;
    }
    int ran = 0;
    int failedCases = 0;
    for (const TestCase& c : registry()) {
        if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
        state().failuresInCase = 0;
        const long checksBefore = state().checks;
        try {
            c.fn();
        } catch (const AbortCase&) {
            // REQUIRE already reported the failure.
        } catch (const std::exception& e) {
            fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            fail(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        ++ran;
        const bool ok = state().failuresInCase == 0;
        if (!ok) ++failedCases;
        std::printf("  [%s] %-52s %6ld checks\n", ok ? " OK " : "FAIL", c.name, state().checks - checksBefore);
        std::fflush(stdout);
    }
    std::printf("%d case(s), %ld check(s), %ld failure(s)\n", ran, state().checks, state().failures);
    if (ran == 0) {
        std::fprintf(stderr, "no test case matched\n");
        return 2;
    }
    return failedCases == 0 ? 0 : 1;
}

}  // namespace dptest

#define DP_TEST(name)                                                        \
    static void dp_test_##name();                                            \
    static const ::dptest::Registrar dp_registrar_##name(#name, &dp_test_##name); \
    static void dp_test_##name()

#define CHECK(condition)                                                         \
    do {                                                                         \
        ++::dptest::state().checks;                                              \
        if (!(condition)) ::dptest::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
    } while (0)

#define REQUIRE(condition)                                                      \
    do {                                                                        \
        ++::dptest::state().checks;                                             \
        if (!(condition)) {                                                     \
            ::dptest::fail(__FILE__, __LINE__, "REQUIRE(" #condition ")");      \
            throw ::dptest::AbortCase{};                                        \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b) ::dptest::checkEqual((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tolerance) \
    ::dptest::checkNear(static_cast<double>(a), static_cast<double>(b), static_cast<double>(tolerance), #a, #b, __FILE__, __LINE__)

#define DP_TEST_MAIN() \
    int main(int argc, char** argv) { return ::dptest::runAll(argc, argv); }
