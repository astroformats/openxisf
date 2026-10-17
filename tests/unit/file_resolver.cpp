// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// file_resolver(): path() locations to files, confined to the directory of the header file (spec §10.3), and the
// traversals that it refuses.

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "support/bytes.h"
#include "support/files.h"
#include "support/links.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::external_reference;
using openxisf::location_form;
using openxisf::test::bytes;
using openxisf::test::path_of;
using openxisf::test::temp_directory;
using openxisf::test::throws;
using openxisf::test::utf8;
using openxisf::test::write_file;

// A header directory, unit, with a file and a subdirectory, next to a file that is outside it.
class file_resolver_test : public testing::Test
{
protected:
    file_resolver_test()
    {
        std::filesystem::create_directories(unit_directory / "sub");
        write_file(unit_directory / "data.xisb", bytes("inside"));
        write_file(unit_directory / "sub" / "more.dat", bytes("deeper"));
        write_file(root.path() / "outside.dat", bytes("outside"));
    }

    [[nodiscard]] std::string directory() const
    {
        return utf8(unit_directory);
    }

    // The content of the file that the resolver opens for a relative path.
    [[nodiscard]] static std::string open(std::string_view relative, const openxisf::external_resolver& resolver)
    {
        const std::unique_ptr<openxisf::input_source> source =
            resolver(external_reference{.form = location_form::relative_path, .location = std::string(relative)});
        if (!source) {
            ADD_FAILURE() << "no source for " << relative;
            return {};
        }
        std::vector<std::byte> data(source->size());
        source->read(0, data);
        return openxisf::test::text(data);
    }

    [[nodiscard]] std::string open(std::string_view relative) const
    {
        return open(relative, openxisf::file_resolver(directory()));
    }

    template <typename Error> [[nodiscard]] testing::AssertionResult refuses(std::string_view relative, errc code) const
    {
        return throws<Error>(code, [this, relative] { (void)open(relative); });
    }

    temp_directory root;
    std::filesystem::path unit_directory = root.path() / "unit";
};

TEST_F(file_resolver_test, opens_a_file_inside_the_directory)
{
    EXPECT_EQ(open("data.xisb"), "inside");
    EXPECT_EQ(open("sub/more.dat"), "deeper");
    EXPECT_EQ(open("./data.xisb"), "inside");
    EXPECT_EQ(open("sub//../data.xisb"), "inside");
}

TEST_F(file_resolver_test, opens_a_directory_of_any_name)
{
    const std::filesystem::path directory = root.path() / path_of("Nebulosa del Ñandú (星雲)");
    std::filesystem::create_directory(directory);
    write_file(directory / path_of("ñandú.xisb"), bytes("named"));
    EXPECT_EQ(open("ñandú.xisb", openxisf::file_resolver(utf8(directory))), "named");
}

TEST_F(file_resolver_test, refuses_a_path_that_leads_outside_the_directory)
{
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("../outside.dat", errc::location_not_allowed));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("sub/../../outside.dat", errc::location_not_allowed));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("../unit/../outside.dat", errc::location_not_allowed));
    // The directory itself is not inside it.
    EXPECT_TRUE(refuses<openxisf::unsupported_error>(".", errc::location_not_allowed));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("../unit", errc::location_not_allowed));
}

TEST_F(file_resolver_test, refuses_a_file_that_does_not_exist)
{
    EXPECT_TRUE(refuses<openxisf::io_error>("missing.xisb", errc::open_failed));
    EXPECT_TRUE(refuses<openxisf::io_error>("../missing.xisb", errc::open_failed));
    EXPECT_TRUE(throws<openxisf::io_error>(errc::open_failed, [this] {
        (void)open("data.xisb", openxisf::file_resolver(utf8(root.path() / "missing")));
    }));
}

TEST_F(file_resolver_test, a_trailing_separator_names_a_directory)
{
    // The directory of the header file with one is the same directory; a location with one names no regular file.
    EXPECT_EQ(open("data.xisb", openxisf::file_resolver(directory() + "/")), "inside");
    EXPECT_TRUE(refuses<openxisf::io_error>("data.xisb/", errc::open_failed));
    EXPECT_TRUE(refuses<openxisf::io_error>("sub/", errc::not_a_regular_file));
    // Nor does a location whose last step is . or .., and such a step follows a directory, as POSIX resolves a path
    // (spec §10.3): Windows drops these steps from the text of a path, and the realpath() of macOS skips a . after a
    // file.
    EXPECT_TRUE(refuses<openxisf::io_error>("data.xisb/.", errc::open_failed));
    EXPECT_TRUE(refuses<openxisf::io_error>("data.xisb/..", errc::open_failed));
    EXPECT_TRUE(refuses<openxisf::io_error>("sub/.", errc::not_a_regular_file));
    EXPECT_TRUE(refuses<openxisf::io_error>("sub/more.dat/../more.dat", errc::open_failed));
    EXPECT_TRUE(refuses<openxisf::io_error>("sub/more.dat/./", errc::open_failed));
    EXPECT_EQ(open("sub/./../sub/more.dat"), "deeper");
}

TEST_F(file_resolver_test, a_name_in_another_case_names_the_file_where_the_file_system_ignores_case)
{
    std::error_code error;
    if (!std::filesystem::exists(unit_directory / "DATA.XISB", error)) {
        EXPECT_TRUE(refuses<openxisf::io_error>("DATA.XISB", errc::open_failed));
        return;
    }
    EXPECT_EQ(open("DATA.XISB"), "inside");
    // The directory of the header file in another case is the same directory, and confines as it does.
    const openxisf::external_resolver resolver = openxisf::file_resolver(utf8(root.path() / "UNIT"));
    EXPECT_EQ(open("data.xisb", resolver), "inside");
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::location_not_allowed,
                                                    [&resolver] { (void)open("../outside.dat", resolver); }));
}

TEST_F(file_resolver_test, refuses_what_is_not_a_regular_file)
{
    EXPECT_TRUE(refuses<openxisf::io_error>("sub", errc::not_a_regular_file));
#if defined(_WIN32)
    // Device names, which Windows reserves in every directory, are refused before they are opened, as a device that
    // cannot be opened is too, such as a serial port that the machine does not have. Since Windows 11, only NUL is one.
    for (const std::string_view device : {"NUL", "CON", "COM1", "COM9", "aux", "nul.txt", "LPT1.xisb"}) {
        const errc code = openxisf::test::is_device_name(utf8(unit_directory / path_of(device)))
                              ? errc::not_a_regular_file
                              : errc::open_failed;
        EXPECT_TRUE(refuses<openxisf::io_error>(device, code)) << device;
    }
#else
    ASSERT_EQ(::mkfifo((unit_directory / "fifo").c_str(), 0600), 0);
    EXPECT_TRUE(refuses<openxisf::io_error>("fifo", errc::not_a_regular_file));
#endif
}

TEST_F(file_resolver_test, follows_symbolic_links_only_inside_the_directory)
{
    if (!openxisf::test::create_symbolic_link(unit_directory / "data.xisb", unit_directory / "inside.xisb")) {
        GTEST_SKIP() << "the system does not let this process create symbolic links";
    }
    EXPECT_EQ(open("inside.xisb"), "inside");
    ASSERT_TRUE(openxisf::test::create_symbolic_link(root.path() / "outside.dat", unit_directory / "escape.xisb"));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("escape.xisb", errc::location_not_allowed));
    ASSERT_TRUE(openxisf::test::create_symbolic_link(root.path(), unit_directory / "up"));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("up/outside.dat", errc::location_not_allowed));
    // A link that leaves and comes back stays inside.
    EXPECT_EQ(open("up/unit/data.xisb"), "inside");
}

#if defined(_WIN32)
TEST_F(file_resolver_test, follows_junctions_only_inside_the_directory)
{
    openxisf::test::create_junction(root.path(), unit_directory / "up");
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("up/outside.dat", errc::location_not_allowed));
    EXPECT_EQ(open("up/unit/data.xisb"), "inside");
    openxisf::test::create_junction(unit_directory / "sub", unit_directory / "down");
    EXPECT_EQ(open("down/more.dat"), "deeper");
}

TEST_F(file_resolver_test, refuses_what_names_no_file_on_windows)
{
    EXPECT_TRUE(refuses<openxisf::unsupported_error>(R"(sub\more.dat)", errc::unsupported_location));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>(R"(..\outside.dat)", errc::unsupported_location));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("data.xisb:stream", errc::unsupported_location));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("C:/outside.dat", errc::unsupported_location));
}

TEST_F(file_resolver_test, the_names_that_windows_trims_are_confined_like_the_others)
{
    // Windows drops the dots and spaces that end a path, so these name data.xisb, and the file that a path names is
    // the one confined.
    EXPECT_EQ(open("data.xisb."), "inside");
    EXPECT_EQ(open("data.xisb  "), "inside");
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("../outside.dat.", errc::location_not_allowed));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("../outside.dat ", errc::location_not_allowed));
}

TEST_F(file_resolver_test, the_file_read_is_the_one_whose_canonical_path_was_checked)
{
    // A directory dot. inside, whose canonical path ends with a dot, which Windows drops from the steps of a path that
    // it normalizes, beside a junction dot that leads outside: the file read through the junction j to dot. is the
    // one in dot., not the one that its canonical path names once normalized.
    const std::filesystem::path outside = root.path() / "outside";
    std::filesystem::create_directory(outside);
    write_file(outside / "data.xisb", bytes("outside"));
    std::filesystem::create_directory(openxisf::test::long_form(unit_directory / "dot."));
    write_file(openxisf::test::long_form(unit_directory / "dot." / "data.xisb"), bytes("inside, in dot."));
    openxisf::test::create_junction(outside, unit_directory / "dot");
    openxisf::test::create_junction(unit_directory / "dot.", unit_directory / "j");

    EXPECT_EQ(open("j/data.xisb"), "inside, in dot.");
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("dot/data.xisb", errc::location_not_allowed));
    EXPECT_TRUE(refuses<openxisf::unsupported_error>("dot./data.xisb", errc::location_not_allowed));
}

TEST_F(file_resolver_test, the_short_name_of_a_directory_reads_the_file_found_in_it)
{
    // The 8.3 alias of a directory longername. inside, beside a junction longername that leads outside: the file read
    // is the one in longername., which the alias names, not the one that its canonical path names once normalized.
    const std::filesystem::path outside = root.path() / "outside";
    std::filesystem::create_directory(outside);
    write_file(outside / "data.xisb", bytes("outside"));
    const std::filesystem::path dotted = openxisf::test::long_form(unit_directory / "longername.");
    std::filesystem::create_directory(dotted);
    write_file(dotted / "data.xisb", bytes("inside, in longername."));
    openxisf::test::create_junction(outside, unit_directory / "longername");

    std::wstring alias(MAX_PATH, L'\0');
    const DWORD size = GetShortPathNameW(dotted.c_str(), alias.data(), static_cast<DWORD>(alias.size()));
    alias.resize(size < alias.size() ? size : 0);
    const std::filesystem::path name = std::filesystem::path(alias).filename();
    if (name.empty() || name == L"longername.") {
        GTEST_SKIP() << "the volume gives no short names";
    }
    EXPECT_EQ(open(utf8(name) + "/data.xisb"), "inside, in longername.");
}
#endif

TEST_F(file_resolver_test, a_relative_directory_is_taken_from_the_current_directory_when_the_resolver_is_made)
{
    // The resolver of a unit opened by a relative path finds its files when the unit is read again from another current
    // directory.
    openxisf::external_resolver resolver;
    {
        const openxisf::test::working_directory moved(root.path());
        resolver = openxisf::file_resolver("unit");
    }
    const openxisf::test::working_directory moved(unit_directory / "sub");
    EXPECT_EQ(open("data.xisb", resolver), "inside");
}

TEST_F(file_resolver_test, refuses_absolute_paths_unless_allowed)
{
    // The absolute path of outside.dat in UNIX syntax: /c/... on Windows.
    std::string path = utf8(std::filesystem::absolute(root.path() / "outside.dat"));
#if defined(_WIN32)
    for (char& c : path) {
        c = c == '\\' ? '/' : c;
    }
    ASSERT_EQ(path[1], ':');
    path = "/" + std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])))) + path.substr(2);
#endif
    const external_reference reference{.form = location_form::absolute_path, .location = path};
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::location_not_allowed,
                                                    [&] { (void)openxisf::file_resolver(directory())(reference); }));
    const std::unique_ptr<openxisf::input_source> source =
        openxisf::file_resolver(directory(), {.absolute_paths = true})(reference);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->size(), 7U);
}

TEST_F(file_resolver_test, refuses_urls)
{
    const external_reference reference{.form = location_form::url, .location = "file:///data/unit.xisb"};
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::unsupported_location, [&] {
        (void)openxisf::file_resolver(directory(), {.absolute_paths = true})(reference);
    }));
}

TEST(file_resolver, needs_a_valid_directory)
{
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [] { (void)openxisf::file_resolver(""); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_utf8, [] { (void)openxisf::file_resolver("a\xFF"); }));
}

} // namespace
