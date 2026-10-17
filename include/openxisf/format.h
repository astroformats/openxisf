// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>
#include <openxisf/property.h>

#include <cstdint>
#include <string>

/// @file
/// Property values as text, with property format specifiers (spec §8.4.3).

namespace openxisf {

/// The text of a property value, as format asks for it (spec §8.4.3, §11.1.2).
///
/// - Integers in the base of the format, Booleans as words or as 0 and 1, and floating point values in the notation and
///   precision of the format, automatic notation being the g conversion of printf. Non-finite values are inf and nan.
/// - A sign as the format says for numbers, never for a value represented as zero.
/// - Padding with the fill character to the width of the format, with its alignment. The width counts the characters of
///   a string, not its bytes.
/// - Strings take only the width, the fill character and the alignment.
/// - The parts of a complex number, the elements of a vector and those of a matrix are each formatted on their own.
///   A complex number is written (real,imaginary); the elements of a vector are separated by commas, and the rows of a
///   matrix by semicolons.
/// - A TimePoint takes no format (spec §8.4.3.1): it is written as an ISO 8601 date and time in UTC, such as
///   2026-03-14T01:59:26.535Z, the years -1 and 10000 that a reader returns for times with an offset from UTC included.
///
/// The unit of the format is not appended. A width or a precision above 1024 counts as 1024, so that the format of a
/// property read from a file cannot make a component longer than a few kilobytes. It applies to each element of a
/// vector or a matrix, so the text of a long one can take far more than its value: that of a vector of 64 MiB of
/// bytes, within limits::max_ancillary_data, with a width of 1024, takes 64 GiB. format_element() writes one element
/// at a time. Float128 values are formatted from their nearest double (to_double()).
/// @throws usage_error for a TimePoint that is not a valid date and time.
[[nodiscard]] OPENXISF_API std::string format_value(const property_value& value, const property_format& format = {});

/// The text of the element at index of a vector or matrix value, in row order for a matrix, as format_value() writes
/// it. An application that shows the first elements of a long vector needs no text for the others.
/// @throws usage_error when value is not a vector or a matrix, or index is not below its length.
[[nodiscard]] OPENXISF_API std::string format_element(const property_value& value, std::uint64_t index,
                                                      const property_format& format = {});

} // namespace openxisf
