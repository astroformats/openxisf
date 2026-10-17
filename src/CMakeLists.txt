# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.h.in"
    "${PROJECT_BINARY_DIR}/include/openxisf/version.h"
    @ONLY)

# Whether the library is static, in an installed header that export.h includes: an application that takes only the
# include and library directories of the package, as MSBuild and hand-written makefiles do, needs no definition.
if(OPENXISF_SHARED)
    set(OPENXISF_STATIC_DEFINE OFF)
else()
    set(OPENXISF_STATIC_DEFINE ON)
endif()
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/linkage.h.in"
    "${PROJECT_BINARY_DIR}/include/openxisf/linkage.h"
    @ONLY)

# The internal code, in namespace openxisf::detail.
set(internal_sources
    algorithms/astrometric_model.cpp
    algorithms/celestial_sphere.cpp
    algorithms/distortion_model.cpp
    algorithms/pixel_values.cpp
    codec/compressed_block.cpp
    codec/compression.cpp
    codec/lz4_codec.cpp
    codec/shuffle.cpp
    codec/zlib_codec.cpp
    codec/zstd_codec.cpp
    container/block_attributes.cpp
    container/blocks_file.cpp
    container/data_block.cpp
    container/external_blocks.cpp
    container/file_layout.cpp
    container/unit_writer.cpp
    core/allocation.cpp
    core/checked_math.cpp
    core/data_encoding.cpp
    core/diagnostic_log.cpp
    core/endian.cpp
    core/parallel.cpp
    core/quote.cpp
    core/text_grammar.cpp
    core/utc_time.cpp
    core/utf16.cpp
    core/utf8.cpp
    core/uuid.cpp
    core/xoshiro.cpp
    crypto/hash.cpp
    crypto/sha1.cpp
    crypto/sha2.cpp
    crypto/sha3.cpp
    io/paths.cpp
    io/range_check.cpp
    io/thread_safe_source.cpp
    model/ancillary.cpp
    model/ancillary_attributes.cpp
    model/ancillary_budget.cpp
    model/format_specifier.cpp
    model/header.cpp
    model/header_tree.cpp
    model/held_size.cpp
    model/image_attributes.cpp
    model/images.cpp
    model/outline.cpp
    model/pixel_layout.cpp
    model/pixels.cpp
    model/properties.cpp
    model/property_catalog.cpp
    model/property_data.cpp
    model/property_text.cpp
    model/property_types.cpp
    model/tables.cpp
    model/unit.cpp
    model/validation.cpp
    model/value_reader.cpp
    xml/namespaces.cpp
    xml/xml_document.cpp
    xml/xml_writer.cpp)
if(WIN32)
    list(APPEND internal_sources io/native_file_win32.cpp)
else()
    list(APPEND internal_sources io/native_file_posix.cpp)
endif()

# The public API.
set(public_sources
    error.cpp
    algorithms/astrometry.cpp
    algorithms/color.cpp
    algorithms/display.cpp
    algorithms/format.cpp
    algorithms/orientation.cpp
    image.cpp
    property.cpp
    reader.cpp
    version.cpp
    writer.cpp
    io/callback.cpp
    io/file.cpp
    io/file_resolver.cpp
    io/interfaces.cpp
    io/memory.cpp)

add_library(openxisf ${public_sources} ${internal_sources})
add_library(openxisf::openxisf ALIAS openxisf)

target_include_directories(openxisf
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(openxisf PUBLIC cxx_std_20)

set_target_properties(openxisf PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DEFINE_SYMBOL OPENXISF_EXPORTS
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${OPENXISF_SOVERSION}"
    # The Debug library has a name of its own, so that Debug and Release builds install side by side in one prefix.
    DEBUG_POSTFIX d)

# windows.h without the min and max macros, which break the standard headers included after it.
set(windows_definitions NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_compile_definitions(openxisf PRIVATE ${windows_definitions})
endif()

# The dependencies are private: no third-party type appears in an installed header.
set(private_dependencies pugixml::pugixml ZLIB::ZLIB lz4::lz4 zstd::libzstd)
if(OPENXISF_WITH_TBB)
    list(APPEND private_dependencies TBB::tbb)
endif()
if(OPENXISF_WITH_OPENSSL)
    list(APPEND private_dependencies OpenSSL::Crypto)
endif()
target_link_libraries(openxisf PRIVATE ${private_dependencies})

# The optional parts of the internal code, which the tests and benchmarks see too.
set(internal_definitions "")
foreach(option OPENXISF_ENABLE_SIMD OPENXISF_WITH_TBB OPENXISF_WITH_OPENSSL)
    if(${option})
        list(APPEND internal_definitions ${option})
    endif()
endforeach()
target_compile_definitions(openxisf PRIVATE ${internal_definitions})

openxisf_enable_warnings(openxisf)
openxisf_enable_sanitizers(openxisf)
if(OPENXISF_HARDENING)
    openxisf_enable_hardening(openxisf)
endif()

# The library and its internal code, for the tests and the fuzz targets. A shared library exports the public API only,
# so they compile the internal code themselves; it has no state, so the second copy is harmless. The internal headers
# include those of the dependencies.
add_library(openxisf_internal INTERFACE)
target_include_directories(openxisf_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(openxisf_internal INTERFACE openxisf::openxisf ${private_dependencies})
target_compile_definitions(openxisf_internal INTERFACE ${internal_definitions})
if(OPENXISF_SHARED)
    list(TRANSFORM internal_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    target_sources(openxisf_internal INTERFACE ${internal_sources})
    if(WIN32)
        target_compile_definitions(openxisf_internal INTERFACE ${windows_definitions})
    endif()
endif()
