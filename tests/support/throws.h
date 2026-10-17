// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>

#include <gtest/gtest.h>

#include <exception>
#include <utility>

namespace openxisf::test {

/// Success when function throws Error with the given code. Use it as EXPECT_TRUE(throws<Error>(code, function)).
template <typename Error, typename Function>
[[nodiscard]] testing::AssertionResult throws(errc code, Function&& function)
{
    try {
        std::forward<Function>(function)();
    } catch (const Error& failure) {
        if (failure.code() == code) {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure() << "threw code " << static_cast<int>(failure.code()) << " instead of "
                                           << static_cast<int>(code) << ": " << failure.what();
    } catch (const std::exception& other) {
        return testing::AssertionFailure() << "threw another exception: " << other.what();
    }
    return testing::AssertionFailure() << "threw nothing";
}

} // namespace openxisf::test
