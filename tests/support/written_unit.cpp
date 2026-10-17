// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "support/written_unit.h"

#include <openxisf/io.h>

#include "support/environment.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "xml/xml_document.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace openxisf::test {

std::string header_of(std::span<const std::byte> file)
{
    if (file.size() < 16) {
        ADD_FAILURE() << "a monolithic file of " << file.size() << " bytes has no header";
        return {};
    }
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        length |= std::to_integer<std::uint64_t>(file[8 + i]) << (8U * i);
    }
    if (length > file.size() - 16) {
        ADD_FAILURE() << "the header length " << length << " goes beyond the file of " << file.size() << " bytes";
        return {};
    }
    std::string header(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        header[i] = static_cast<char>(file[16 + i]);
    }
    return header;
}

std::unique_ptr<pugi::xml_document> parsed_header(std::span<const std::byte> file)
{
    return detail::parse_xml(header_of(file), 16, {});
}

namespace {

// Records a unit with write in the directory of OPENXISF_WRITTEN_UNITS_DIR, when it is set: write receives the
// directory and the name of the unit, after the running test. Several units of one test are numbered in order, also
// when threads write them.
void record(const std::function<void(const std::filesystem::path& directory, const std::string& base)>& write)
{
    const std::optional<std::string> directory = environment_variable("OPENXISF_WRITTEN_UNITS_DIR");
    if (!directory) {
        return;
    }
    static std::mutex mutex;
    static std::string last_test;
    static int count = 0;
    const std::scoped_lock lock(mutex);
    const testing::TestInfo* test = testing::UnitTest::GetInstance()->current_test_info();
    std::string name = test == nullptr ? "unknown" : std::string(test->test_suite_name()) + "." + test->name();
    count = name == last_test ? count + 1 : 1;
    last_test = name;
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            c = '_';
        }
    }
    write(path_of(*directory), name + "." + std::to_string(count));
}

} // namespace

void record_unit(std::span<const std::byte> file)
{
    record([file](const std::filesystem::path& directory, const std::string& base) {
        const std::string header = header_of(file);
        write_file(directory / path_of(base + ".xml"), std::as_bytes(std::span(header.data(), header.size())));
        write_file(directory / path_of(base + ".xisf"), file);
    });
}

void record_distributed_unit(const distributed_unit& unit, std::string_view blocks_path)
{
    record([&unit, blocks_path](const std::filesystem::path& directory, const std::string& base) {
        write_file(directory / path_of(base + ".xml"), unit.header);
        const std::filesystem::path blocks = directory / path_of(base) / path_of(std::string(blocks_path));
        std::filesystem::create_directories(blocks.parent_path());
        write_file(directory / path_of(base) / "unit.xish", unit.header);
        write_file(blocks, unit.blocks);
    });
}

std::vector<std::byte> written(const writer& output, sink_kind kind)
{
    std::vector<std::byte> file;
    if (kind == sink_kind::rewritable) {
        memory_sink sink;
        output.save(sink);
        file = sink.release();
    } else {
        callback_sink sink(
            [&file](std::span<const std::byte> data) { file.insert(file.end(), data.begin(), data.end()); });
        output.save(sink);
    }
    record_unit(file);
    return file;
}

distributed_unit written_distributed(const writer& output, sink_kind kind)
{
    distributed_unit unit;
    if (kind == sink_kind::rewritable) {
        memory_sink header;
        memory_sink blocks;
        output.save_distributed(header, blocks, blocks_file_name);
        unit = {.header = header.release(), .blocks = blocks.release()};
    } else {
        const auto appender = [](std::vector<std::byte>& target) {
            return
                [&target](std::span<const std::byte> data) { target.insert(target.end(), data.begin(), data.end()); };
        };
        callback_sink header(appender(unit.header));
        callback_sink blocks(appender(unit.blocks));
        output.save_distributed(header, blocks, blocks_file_name);
    }
    record_distributed_unit(unit);
    return unit;
}

reader open_distributed(const distributed_unit& unit, read_options options)
{
    auto blocks = std::make_shared<const std::vector<std::byte>>(unit.blocks);
    options.resolver = [blocks](const external_reference& reference) -> std::unique_ptr<input_source> {
        if (reference.form != location_form::relative_path || reference.location != blocks_file_name) {
            return nullptr;
        }
        return std::make_unique<memory_source>(std::span<const std::byte>(*blocks));
    };
    return reader(std::make_unique<memory_source>(unit.header), std::move(options));
}

pugi::xml_node element_at(const pugi::xml_document& header, std::string_view path)
{
    pugi::xml_node node = header.document_element();
    while (!path.empty() && !node.empty()) {
        const std::size_t slash = path.find('/');
        node = node.child(std::string(path.substr(0, slash)).c_str());
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    }
    return node;
}

} // namespace openxisf::test
