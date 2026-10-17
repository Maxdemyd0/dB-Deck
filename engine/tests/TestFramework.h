#pragma once
// Tiny self-contained test harness (no external dependency).

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace dbtest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Failure {
    std::string message;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace dbtest

#define DB_CONCAT2(a, b) a##b
#define DB_CONCAT(a, b) DB_CONCAT2(a, b)
#define TEST(name)                                                                       \
    static void DB_CONCAT(test_fn_, __LINE__)();                                         \
    static ::dbtest::Registrar DB_CONCAT(test_reg_, __LINE__)(name, DB_CONCAT(test_fn_, __LINE__)); \
    static void DB_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::ostringstream os_;                                                          \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";               \
            throw ::dbtest::Failure{os_.str()};                                              \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        const double a_ = (a), b_ = (b);                                                               \
        if (!(std::fabs(a_ - b_) <= (tol))) {                                                          \
            std::ostringstream os_;                                                                    \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR(" #a ", " #b ") failed: " << a_ << " vs " \
                << b_ << " (tol " << (tol) << ")";                                                     \
            throw ::dbtest::Failure{os_.str()};                                                        \
        }                                                                                              \
    } while (0)

#define CHECK_THROWS(expr)                                                                   \
    do {                                                                                     \
        bool threw_ = false;                                                                 \
        try {                                                                                \
            expr;                                                                            \
        } catch (...) {                                                                      \
            threw_ = true;                                                                   \
        }                                                                                    \
        if (!threw_) throw ::dbtest::Failure{std::string(__FILE__) + ": expected throw: " #expr}; \
    } while (0)
