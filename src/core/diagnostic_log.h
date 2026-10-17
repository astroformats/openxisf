// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace openxisf::detail {

/// The exception of an error that a function of the application raised, such as a resolver, whose class the code does
/// not give: an io_error, with its system error, or an unsupported_error. by_code leaves the class to the code.
struct error_origin
{
    enum class kind : std::uint8_t
    {
        by_code,
        io,
        unsupported,
    };

    kind raised = kind::by_code;
    std::error_code system_code{};
};

/// Throws the exception for an error found in a unit: the class of origin, when it gives one, and otherwise
/// integrity_error for data that fail a check, unsupported_error for valid data that OpenXISF cannot handle or does not
/// allow, limit_error for data beyond a limit, io_error for an external file that cannot be opened or read, and
/// invalid_data_error for anything else.
[[noreturn]] void throw_unit_error(errc code, std::string_view message, error_context context,
                                   const error_origin& origin = {});

/// The diagnostics recorded while a unit is opened (spec §7). In strict mode, an error fails the open instead.
///
/// The log lists at most max_diagnostics of them, since a header can hold a million problems. When there are more, the
/// last one stands for itself and all those after it: it says how many they are, and has the severity of the most
/// serious of them and the code of the first of those, without a context.
class diagnostic_log
{
public:
    static constexpr std::size_t max_diagnostics = 10'000;

    explicit diagnostic_log(bool strict) noexcept : strict_(strict) {}

    /// Something the specification allows was ignored.
    void info(errc code, std::string message, error_context context = {});

    /// A harmless violation of the specification was tolerated.
    void warning(errc code, std::string message, error_context context = {});

    /// An object of the unit is unavailable. Throws the exception of throw_unit_error() instead in strict mode.
    void error(errc code, std::string message, error_context context = {}, const error_origin& origin = {});

    [[nodiscard]] const std::vector<diagnostic>& entries() const noexcept
    {
        return entries_;
    }

    /// Moves the diagnostics out of the log, which is empty afterwards.
    [[nodiscard]] std::vector<diagnostic> release() noexcept;

private:
    void add(openxisf::severity level, errc code, std::string message, error_context context);

    bool strict_;
    std::vector<diagnostic> entries_;
    // The number of problems that the last entry stands for when the log is full, or zero.
    std::uint64_t unlisted_ = 0;
};

} // namespace openxisf::detail
