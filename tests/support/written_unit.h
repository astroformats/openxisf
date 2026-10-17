// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include <pugixml.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Units written by the writer of the library, for its tests. Every unit written in tests can be recorded, with its
// header apart, for the XML schema of XISF, which only CI downloads, and for other XISF readers: set
// OPENXISF_WRITTEN_UNITS_DIR to an existing directory.

namespace openxisf::test {

/// The sinks that the writer writes in different ways.
enum class sink_kind
{
    rewritable,  ///< A memory_sink, which can rewrite: the header is written last.
    append_only, ///< A callback_sink without a rewrite function: the unit is written in order.
};

/// The header of a monolithic file: the bytes that its header length counts after the first 16. Fails the test when
/// the file is too short for them.
[[nodiscard]] std::string header_of(std::span<const std::byte> file);

/// The header of a monolithic file, parsed.
[[nodiscard]] std::unique_ptr<pugi::xml_document> parsed_header(std::span<const std::byte> file);

/// Writes a monolithic file, and its header apart, to the directory of OPENXISF_WRITTEN_UNITS_DIR, when it is set,
/// named after the running test: name.xisf and name.xml.
void record_unit(std::span<const std::byte> file);

/// The unit that output writes into a sink of the given kind, recorded with record_unit().
[[nodiscard]] std::vector<std::byte> written(const writer& output, sink_kind kind = sink_kind::rewritable);

/// The name of the data blocks file of the distributed units that the tests write.
inline constexpr std::string_view blocks_file_name = "unit.xisb";

/// A distributed unit as the writer writes it: a header file, and a data blocks file that the header names
/// blocks_file_name.
struct distributed_unit
{
    std::vector<std::byte> header{};
    std::vector<std::byte> blocks{};
};

/// Writes a distributed unit to the directory of OPENXISF_WRITTEN_UNITS_DIR, when it is set, as record_unit() writes a
/// monolithic file: name.xml, the header, and the unit itself in a directory of that name, its data blocks file at
/// blocks_path from the header file, which the header names.
void record_distributed_unit(const distributed_unit& unit, std::string_view blocks_path = blocks_file_name);

/// The distributed unit that output writes into sinks of the given kind, recorded with record_distributed_unit().
[[nodiscard]] distributed_unit written_distributed(const writer& output, sink_kind kind = sink_kind::rewritable);

/// Opens a distributed unit in memory, with a resolver that finds its data blocks file.
[[nodiscard]] reader open_distributed(const distributed_unit& unit, read_options options = {});

/// The first element at a path of element names below the root element, such as "Image/Property", or an empty node.
[[nodiscard]] pugi::xml_node element_at(const pugi::xml_document& header, std::string_view path);

} // namespace openxisf::test
