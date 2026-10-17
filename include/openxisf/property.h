// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>

#include <chrono>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// Properties: identifiers associated with typed values (spec §8.4).

namespace openxisf {

/// An unsigned 128-bit integer: high * 2^64 + low (spec §8.1).
struct uint128
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator==(const uint128&, const uint128&) = default;
};

/// A signed 128-bit integer in two's complement: high * 2^64 + low (spec §8.1).
struct int128
{
    std::int64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator==(const int128&, const int128&) = default;
};

/// An IEEE 754 binary128 value (spec §8.1), as its bit pattern: high * 2^64 + low. OpenXISF has no quadruple-precision
/// arithmetic; to_double() converts a value to the nearest double.
struct float128
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator==(const float128&, const float128&) = default;
};

/// A complex number whose parts are Float128 values (spec §8.4.4.2).
struct complex128
{
    float128 real{};
    float128 imag{};

    friend bool operator==(const complex128&, const complex128&) = default;
};

/// The double nearest to value, rounded to even: an infinity beyond the range of double, and a NaN for a NaN.
[[nodiscard]] OPENXISF_API double to_double(float128 value) noexcept;

/// An instant in UTC: the value of a TimePoint property (spec §8.4.4.4). A TimePoint written with a UTC offset is read
/// as the same instant in UTC; a fraction of a second beyond nanoseconds is truncated. The instant of a TimePoint near
/// the ends of its years, 0000 to 9999, can fall in year -1 or 10000 in UTC, as that of 9999-12-31T23:30:00-01:00
/// does; the writer, which writes TimePoints in UTC, refuses such an instant (errc::invalid_time_point).
struct date_time
{
    int year = 1970;
    unsigned month = 1;           ///< 1 to 12.
    unsigned day = 1;             ///< 1 to the length of the month.
    unsigned hour = 0;            ///< 0 to 23.
    unsigned minute = 0;          ///< 0 to 59.
    unsigned second = 0;          ///< 0 to 60, where 60 is a leap second.
    std::uint32_t nanosecond = 0; ///< 0 to 999,999,999.

    /// Chronological order.
    friend auto operator<=>(const date_time&, const date_time&) = default;
};

/// The date_time of a time point of the system clock, such as std::chrono::system_clock::now(). The system clock counts
/// no leap seconds, so the second is never 60.
[[nodiscard]] OPENXISF_API date_time to_date_time(std::chrono::sys_time<std::chrono::nanoseconds> time) noexcept;

/// The types of property values (spec §8.4.4). A table property is a table, serialized as a Table element, so it has no
/// type here.
enum class property_type : std::uint8_t
{
    boolean,
    int8,
    uint8,
    int16,
    uint16,
    int32,
    uint32,
    int64,
    uint64,
    int128,
    uint128,
    float32,
    float64,
    float128,
    complex32,
    complex64,
    complex128,
    string,
    time_point,
    i8_vector,
    ui8_vector,
    i16_vector,
    ui16_vector,
    i32_vector,
    ui32_vector,
    i64_vector,
    ui64_vector,
    i128_vector,
    ui128_vector,
    f32_vector,
    f64_vector,
    f128_vector,
    c32_vector,
    c64_vector,
    c128_vector,
    i8_matrix,
    ui8_matrix,
    i16_matrix,
    ui16_matrix,
    i32_matrix,
    ui32_matrix,
    i64_matrix,
    ui64_matrix,
    i128_matrix,
    ui128_matrix,
    f32_matrix,
    f64_matrix,
    f128_matrix,
    c32_matrix,
    c64_matrix,
    c128_matrix,
};

/// The name of a property type in the specification, such as "Float32" or "UI16Vector"; empty for a value that is no
/// enumerator of property_type.
[[nodiscard]] OPENXISF_API std::string_view property_type_name(property_type type) noexcept;

/// Alignment of padded representations (spec §8.4.3).
enum class format_align : std::uint8_t
{
    right,
    left,
    center,
};

/// When numeric representations have a sign.
enum class format_sign : std::uint8_t
{
    automatic, ///< A minus sign for values below zero.
    force,     ///< A minus sign below zero and a plus sign above.
};

/// The notation of floating point representations.
enum class format_notation : std::uint8_t
{
    automatic, ///< As the g conversion of printf.
    scientific,
    fixed,
};

/// The representation of Boolean values.
enum class format_bool : std::uint8_t
{
    alpha,   ///< false and true.
    numeric, ///< 0 and 1.
};

/// The base of integer representations.
enum class format_base : std::uint8_t
{
    decimal,
    binary,
    octal,
    hexadecimal,
};

/// A property format specifier (spec §8.4.3): how to represent a value as text. Every member starts with the default of
/// the specification, so a format that leaves a member out means its default.
struct property_format
{
    /// The minimum length of a representation, reached with fill characters; 0 means no padding.
    std::uint32_t width = 0;
    /// A printable ASCII character other than the semicolon.
    char fill = ' ';
    format_align align = format_align::right;
    format_sign sign = format_sign::automatic;
    /// Digits after the decimal separator, or significant digits in automatic notation.
    std::uint32_t precision = 6;
    format_notation notation = format_notation::automatic;
    format_bool boolean = format_bool::alpha;
    format_base base = format_base::decimal;
    /// The unit of the value, such as "m/s"; empty when there is none.
    std::string unit{};

    friend bool operator==(const property_format&, const property_format&) = default;
};

/// The C++ types of the elements of vector and matrix properties. Integer elements are of the fixed-width types alone,
/// std::int8_t to std::uint64_t, which are other C++ types on other platforms: a std::vector of long, long long or
/// std::size_t is a vector of one of them on some platforms and of none on others. Convert such a vector to one of
/// the fixed-width types.
template <typename T>
concept property_element =
    std::same_as<T, std::int8_t> || std::same_as<T, std::uint8_t> || std::same_as<T, std::int16_t> ||
    std::same_as<T, std::uint16_t> || std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t> ||
    std::same_as<T, std::int64_t> || std::same_as<T, std::uint64_t> || std::same_as<T, int128> ||
    std::same_as<T, uint128> || std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, float128> ||
    std::same_as<T, std::complex<float>> || std::same_as<T, std::complex<double>> || std::same_as<T, complex128>;

#if defined(_MSC_VER)
// property_value and property_list hold standard-library members, which cl reports for an exported class. A C++
// interface requires the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// The value of a property: a property type and a value of that type (spec §8.4.4).
///
/// Each type is held in one C++ type: Boolean in bool, Int8 to UInt64 in the integers of the same width, Int128 and
/// UInt128 in int128 and uint128, Float32 and Float64 in float and double, Complex32 and Complex64 in std::complex,
/// String in a std::string of UTF-8, TimePoint in date_time, and vectors and matrices in a std::vector of their
/// elements, in native byte order and, for matrices, in row order. Float128 and Complex128 scalars are kept as the text
/// of their value attribute, since OpenXISF has no quadruple-precision arithmetic, in a form of spec §8.3.3 that
/// std::from_chars may refuse, such as +.5, or find beyond the range of double; Float128 and Complex128 elements of
/// vectors and matrices are float128 and complex128 values.
///
/// The constructors take the C++ type of a value and give the property type it maps to: a float makes a Float32 value,
/// a std::vector<std::uint16_t> a UI16Vector. A default-constructed value is an empty String.
class OPENXISF_API property_value
{
public:
    /// Every value, as the C++ type that holds it.
    using storage =
        std::variant<std::string, bool, std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
                     std::uint32_t, std::int64_t, std::uint64_t, int128, uint128, float, double, std::complex<float>,
                     std::complex<double>, date_time, std::vector<std::int8_t>, std::vector<std::uint8_t>,
                     std::vector<std::int16_t>, std::vector<std::uint16_t>, std::vector<std::int32_t>,
                     std::vector<std::uint32_t>, std::vector<std::int64_t>, std::vector<std::uint64_t>,
                     std::vector<int128>, std::vector<uint128>, std::vector<float>, std::vector<double>,
                     std::vector<float128>, std::vector<std::complex<float>>, std::vector<std::complex<double>>,
                     std::vector<complex128>>;

    property_value() = default;
    property_value(const property_value& other);
    property_value(property_value&&) noexcept = default;

    /// Copies other. When the copy throws, this value is left as it was.
    property_value& operator=(const property_value& other)
    {
        property_value copy(other);
        *this = std::move(copy);
        return *this;
    }

    property_value& operator=(property_value&&) noexcept = default;
    ~property_value() = default;

    property_value(bool value) noexcept;
    property_value(std::int8_t value) noexcept;
    property_value(std::uint8_t value) noexcept;
    property_value(std::int16_t value) noexcept;
    property_value(std::uint16_t value) noexcept;
    property_value(std::int32_t value) noexcept;
    property_value(std::uint32_t value) noexcept;
    property_value(std::int64_t value) noexcept;
    property_value(std::uint64_t value) noexcept;

    /// An integer of another C++ type, such as long long where std::int64_t is long: the integer type of its width and
    /// signedness. The character types are not taken for integers here, but a character is promoted to an int, or a
    /// char32_t to an unsigned int, and makes an Int32 or a UInt32 value: set("Instrument:Filter", 'R') sets the
    /// number 82, where "R" sets a String.
    template <typename T>
        requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>) &&
                 (!std::same_as<T, wchar_t>) && (!std::same_as<T, char8_t>) && (!std::same_as<T, char16_t>) &&
                 (!std::same_as<T, char32_t>)
    property_value(T value) noexcept : property_value(fixed_width(value))
    {}

    property_value(int128 value) noexcept;
    property_value(uint128 value) noexcept;
    property_value(float value) noexcept;
    property_value(double value) noexcept;
    property_value(std::complex<float> value) noexcept;
    property_value(std::complex<double> value) noexcept;

    /// A String value. The text must be valid UTF-8 without U+0000 when the value is written.
    property_value(std::string value) noexcept;
    property_value(std::string_view value);
    property_value(const char* value);
    property_value(std::nullptr_t) = delete;

    property_value(date_time value) noexcept;

    /// A vector value.
    template <property_element T>
    property_value(std::vector<T> elements) noexcept
        : type_(vector_type<T>()), value_(std::in_place_type<std::vector<T>>, std::move(elements))
    {}

    /// A matrix value, with its elements in row order.
    /// @throws usage_error when elements does not have rows * columns elements.
    template <property_element T>
    [[nodiscard]] static property_value matrix(std::uint64_t rows, std::uint64_t columns, std::vector<T> elements)
    {
        check_matrix_size(rows, columns, elements.size());
        property_value result(std::move(elements));
        result.type_ = matrix_type<T>();
        result.rows_ = rows;
        result.columns_ = columns;
        return result;
    }

    /// A Float128 scalar, kept as text: a floating point value of spec §8.3.3 within the range of Float128, without the
    /// white space around it, which encoders do not write (spec §8.3.5). A finite value keeps its spelling, and a
    /// non-finite one is kept as NaN, +Inf or -Inf, the forms that encoders use (spec §8.3.3), whatever its spelling.
    /// @throws usage_error when text is not such a value.
    [[nodiscard]] static property_value from_float128_text(std::string_view text);

    /// A Complex128 scalar, kept as text: (real,imag), with Float128 parts, without the white space around the value
    /// and its parts, each kept as from_float128_text() keeps a value.
    /// @throws usage_error when text is not such a value.
    [[nodiscard]] static property_value from_complex128_text(std::string_view text);

    /// The property type of the value.
    [[nodiscard]] property_type type() const noexcept
    {
        return type_;
    }

    /// The value, which must be held in T: get<float>() for a Float32 value, get<std::string>() for a String, Float128
    /// or Complex128 scalar.
    /// @throws usage_error when the value is held in another type.
    template <typename T> [[nodiscard]] const T& get() const
    {
        if (const T* value = std::get_if<T>(&value_)) {
            return *value;
        }
        throw_type_mismatch(type_);
    }

    /// The elements of a vector or matrix value of elements of type T, in row order for a matrix.
    /// @throws usage_error when the value is not such a vector or matrix.
    template <property_element T> [[nodiscard]] std::span<const T> elements() const
    {
        return get<std::vector<T>>();
    }

    /// The number of elements of a vector or matrix value, and 0 for other values.
    [[nodiscard]] std::uint64_t length() const;

    /// The number of rows of a matrix value, and 0 for other values.
    [[nodiscard]] std::uint64_t rows() const noexcept
    {
        return rows_;
    }

    /// The number of columns of a matrix value, and 0 for other values.
    [[nodiscard]] std::uint64_t columns() const noexcept
    {
        return columns_;
    }

    /// The value, as the C++ type that holds it, for std::visit.
    [[nodiscard]] const storage& data() const noexcept
    {
        return value_;
    }

    friend bool operator==(const property_value&, const property_value&) = default;

private:
    property_value(property_type type, storage value) noexcept;

    template <typename T> static auto fixed_width(T value) noexcept
    {
        if constexpr (std::is_signed_v<T>) {
            if constexpr (sizeof(T) == 1) {
                return static_cast<std::int8_t>(value);
            } else if constexpr (sizeof(T) == 2) {
                return static_cast<std::int16_t>(value);
            } else if constexpr (sizeof(T) == 4) {
                return static_cast<std::int32_t>(value);
            } else {
                static_assert(sizeof(T) == 8, "integers wider than 64 bits are int128");
                return static_cast<std::int64_t>(value);
            }
        } else {
            if constexpr (sizeof(T) == 1) {
                return static_cast<std::uint8_t>(value);
            } else if constexpr (sizeof(T) == 2) {
                return static_cast<std::uint16_t>(value);
            } else if constexpr (sizeof(T) == 4) {
                return static_cast<std::uint32_t>(value);
            } else {
                static_assert(sizeof(T) == 8, "integers wider than 64 bits are uint128");
                return static_cast<std::uint64_t>(value);
            }
        }
    }

    template <property_element T> static constexpr property_type vector_type() noexcept
    {
        if constexpr (std::same_as<T, std::int8_t>) {
            return property_type::i8_vector;
        } else if constexpr (std::same_as<T, std::uint8_t>) {
            return property_type::ui8_vector;
        } else if constexpr (std::same_as<T, std::int16_t>) {
            return property_type::i16_vector;
        } else if constexpr (std::same_as<T, std::uint16_t>) {
            return property_type::ui16_vector;
        } else if constexpr (std::same_as<T, std::int32_t>) {
            return property_type::i32_vector;
        } else if constexpr (std::same_as<T, std::uint32_t>) {
            return property_type::ui32_vector;
        } else if constexpr (std::same_as<T, std::int64_t>) {
            return property_type::i64_vector;
        } else if constexpr (std::same_as<T, std::uint64_t>) {
            return property_type::ui64_vector;
        } else if constexpr (std::same_as<T, int128>) {
            return property_type::i128_vector;
        } else if constexpr (std::same_as<T, uint128>) {
            return property_type::ui128_vector;
        } else if constexpr (std::same_as<T, float>) {
            return property_type::f32_vector;
        } else if constexpr (std::same_as<T, double>) {
            return property_type::f64_vector;
        } else if constexpr (std::same_as<T, float128>) {
            return property_type::f128_vector;
        } else if constexpr (std::same_as<T, std::complex<float>>) {
            return property_type::c32_vector;
        } else if constexpr (std::same_as<T, std::complex<double>>) {
            return property_type::c64_vector;
        } else {
            return property_type::c128_vector;
        }
    }

    // The matrix types are in the order of the vector types.
    template <property_element T> static constexpr property_type matrix_type() noexcept
    {
        constexpr int offset = static_cast<int>(property_type::i8_matrix) - static_cast<int>(property_type::i8_vector);
        return static_cast<property_type>(static_cast<int>(vector_type<T>()) + offset);
    }

    static void check_matrix_size(std::uint64_t rows, std::uint64_t columns, std::size_t size);
    [[noreturn]] static void throw_type_mismatch(property_type type);

    property_type type_ = property_type::string;
    storage value_{};
    std::uint64_t rows_ = 0;
    std::uint64_t columns_ = 0;
};

/// A property: an identifier associated with a value, and optionally a format specifier and a comment (spec §8.4).
struct property
{
    /// The identifier: [_a-zA-Z][_a-zA-Z0-9]*, optionally in namespaces separated by colons, such as
    /// "Instrument:ExposureTime" (spec §8.4.1).
    std::string id{};
    property_value value{};
    /// How to represent the value as text; empty when there is none. A format equal to property_format{}, the
    /// defaults of the specification, says nothing, so the writer does not write it, and it reads back empty.
    std::optional<property_format> format{};
    /// A descriptive comment; empty when there is none.
    std::string comment{};

    friend bool operator==(const property&, const property&) = default;
};

/// The properties of one object, in order, each identifier at most once (spec §8.4.1).
///
/// Lookups compare identifiers in linear time; the lists of real units are short.
class OPENXISF_API property_list
{
public:
    using const_iterator = std::vector<property>::const_iterator;

    property_list() = default;
    property_list(const property_list&) = default;
    property_list(property_list&&) noexcept = default;

    /// Copies other. When the copy throws, this list is left as it was.
    property_list& operator=(const property_list& other)
    {
        property_list copy(other);
        *this = std::move(copy);
        return *this;
    }

    property_list& operator=(property_list&&) noexcept = default;
    ~property_list() = default;

    /// The properties of items, in order.
    /// @throws usage_error when two of them have the same identifier.
    explicit property_list(std::vector<property> items);

    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// The property with the given identifier, or null when there is none.
    [[nodiscard]] const property* find(std::string_view id) const noexcept;

    /// True when a property has the given identifier.
    [[nodiscard]] bool contains(std::string_view id) const noexcept;

    /// The property with the given identifier.
    /// @throws usage_error when there is none.
    [[nodiscard]] const property& at(std::string_view id) const;

    /// Adds item at the end, or replaces the property with its identifier, which keeps its position.
    void set(property item);

    /// Adds the property id with the given value, without format or comment, or replaces the property id, which keeps
    /// its position but loses its format and its comment.
    void set(std::string id, property_value value);

    /// Removes the property with the given identifier. True when there was one.
    bool erase(std::string_view id);

    friend bool operator==(const property_list&, const property_list&) = default;

private:
    std::vector<property> properties_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// A field of a table structure (spec §8.4.4.7, §11.2.1): the identifier and the type of a column.
struct table_field
{
    /// A property identifier, unique in its structure.
    std::string id{};
    property_type type = property_type::string;
    /// The title of the column; empty when there is none.
    std::string header{};
    /// How to represent the values of the column as text; empty when there is none. A format equal to
    /// property_format{} is not written, as for a property, and reads back empty.
    std::optional<property_format> format{};

    friend bool operator==(const table_field&, const table_field&) = default;
};

/// A table property (spec §8.4.4.7, §11.3): a two-dimensional array of property values, whose columns are described by
/// the fields of its structure. A table belongs to an image or stands alone, like a property, and its identifier is
/// unique among the properties and tables of its object.
///
/// A plain value: each row is meant to hold one value for each field, of the type of the field, in the order of the
/// fields. The tables that a reader returns are so.
struct table
{
    std::string id{};
    /// The structure: at least one field, each identifier once.
    std::vector<table_field> fields{};
    /// The cells, row by row; a table without rows is empty.
    std::vector<std::vector<property_value>> rows{};
    /// A title for the table; empty when there is none.
    std::string caption{};
    /// A descriptive comment; empty when there is none.
    std::string comment{};

    friend bool operator==(const table&, const table&) = default;
};

} // namespace openxisf
