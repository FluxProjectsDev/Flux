/*
 * Copyright (C) 2024-2026 FebriCahyaa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * @file TestFramework.hpp
 * @brief A ~100-line test harness.
 *
 * Deliberately not GoogleTest: the daemon vendors its dependencies as git submodules and
 * adding a test-only one, for something this small, buys nothing. This gives registration,
 * assertions, and a failure report, which is the whole requirement.
 */

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace flux_test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string &name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

/// Thrown by a failed assertion; caught by the runner so one failure does not abort the run.
struct AssertionFailure {
    std::string message;
};

inline void fail(const std::string &file, int line, const std::string &message) {
    throw AssertionFailure{file + ":" + std::to_string(line) + "  " + message};
}

inline int run_all() {
    int passed = 0;
    std::vector<std::string> failures;

    for (const auto &test : registry()) {
        try {
            test.body();
            std::printf("  \033[32mPASS\033[0m  %s\n", test.name.c_str());
            ++passed;
        } catch (const AssertionFailure &e) {
            std::printf("  \033[31mFAIL\033[0m  %s\n         %s\n", test.name.c_str(), e.message.c_str());
            failures.push_back(test.name);
        } catch (const std::exception &e) {
            std::printf("  \033[31mFAIL\033[0m  %s\n         unexpected exception: %s\n", test.name.c_str(), e.what());
            failures.push_back(test.name);
        }
    }

    std::printf("\n%d passed, %zu failed, %zu total\n", passed, failures.size(), registry().size());
    for (const auto &name : failures) {
        std::printf("  failed: %s\n", name.c_str());
    }
    return failures.empty() ? 0 : 1;
}

} // namespace flux_test

#define FLUX_CONCAT_(a, b) a##b
#define FLUX_CONCAT(a, b) FLUX_CONCAT_(a, b)

/** Define a test case. */
#define TEST(name)                                                                                 \
    static void FLUX_CONCAT(flux_test_fn_, __LINE__)();                                            \
    static ::flux_test::Registrar FLUX_CONCAT(flux_test_reg_, __LINE__)(                           \
        name, FLUX_CONCAT(flux_test_fn_, __LINE__)                                                 \
    );                                                                                             \
    static void FLUX_CONCAT(flux_test_fn_, __LINE__)()

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) ::flux_test::fail(__FILE__, __LINE__, "expected: " #cond);                    \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                        \
    do {                                                                                           \
        if (!(cond)) ::flux_test::fail(__FILE__, __LINE__, std::string(msg) + " (" #cond ")");      \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        /* By value, not by const-ref: binding a reference to a temporary and then formatting  */ \
        /* it in the failure path trips -Wdangling-pointer under -Werror.                      */ \
        const auto lhs_ = (a);                                                                     \
        const auto rhs_ = (b);                                                                     \
        if (!(lhs_ == rhs_)) {                                                                     \
            ::flux_test::fail(                                                                     \
                __FILE__, __LINE__,                                                                \
                std::string("expected " #a " == " #b) + "\n         actual: " +                    \
                    ::flux_test::to_string_(lhs_) + " vs " + ::flux_test::to_string_(rhs_)         \
            );                                                                                     \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
    do {                                                                                           \
        const double lhs_ = static_cast<double>(a);                                                \
        const double rhs_ = static_cast<double>(b);                                                \
        if (std::fabs(lhs_ - rhs_) > (eps)) {                                                      \
            ::flux_test::fail(                                                                     \
                __FILE__, __LINE__,                                                                \
                std::string("expected " #a " ~= " #b) + "\n         actual: " +                    \
                    std::to_string(lhs_) + " vs " + std::to_string(rhs_)                           \
            );                                                                                     \
        }                                                                                          \
    } while (0)

namespace flux_test {

template <typename T>
std::string to_string_(const T &value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else {
        return "<value>";
    }
}

} // namespace flux_test
