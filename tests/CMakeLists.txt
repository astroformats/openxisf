# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

find_package(GTest REQUIRED)
include(GoogleTest)

# A test executable of the given sources, built against the internal library with the warnings, sanitizers and hardening
# of the library.
function(openxisf_add_test_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    # The samples of tests/data, which the tests find by this UTF-8 path.
    target_compile_definitions(${name} PRIVATE OPENXISF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

    target_link_libraries(${name}
        PRIVATE
            openxisf_internal
            GTest::gtest
            GTest::gtest_main)

    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)

    # windows.h without the min and max macros, which break the standard headers included after it.
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    openxisf_enable_warnings(${name})
    openxisf_enable_sanitizers(${name})
    if(OPENXISF_HARDENING)
        openxisf_enable_hardening(${name})
    endif()
endfunction()

openxisf_add_test_executable(openxisf_tests
    concurrency/astrometry.cpp
    concurrency/io.cpp
    concurrency/pixels.cpp
    concurrency/writer.cpp
    conformance/availability.cpp
    conformance/baseline_decoder.cpp
    conformance/baseline_encoder.cpp
    conformance/byte_order.cpp
    conformance/cfa.cpp
    conformance/checksum.cpp
    conformance/compression.cpp
    conformance/display_function.cpp
    conformance/distributed.cpp
    conformance/fits_keyword.cpp
    conformance/header.cpp
    conformance/icc.cpp
    conformance/image.cpp
    conformance/metadata.cpp
    conformance/property_element.cpp
    conformance/reference.cpp
    conformance/resolution.cpp
    conformance/rgbws.cpp
    conformance/signature.cpp
    conformance/table.cpp
    conformance/thumbnail.cpp
    conformance/writer.cpp
    conformance/writer_validation.cpp
    samples/astrometry.cpp
    samples/group_a.cpp
    samples/group_b.cpp
    samples/group_c.cpp
    samples/group_d.cpp
    samples/large.cpp
    samples/rewrite.cpp
    samples/sample_catalog.cpp
    samples/structure.cpp
    support/fixture_builder.cpp
    support/links.cpp
    support/sparse_file.cpp
    support/temp_directory.cpp
    support/virtual_source.cpp
    support/written_unit.cpp
    unit/allocation.cpp
    unit/ancillary_attributes.cpp
    unit/astrometry.cpp
    unit/block_attributes.cpp
    unit/blocks_file.cpp
    unit/callback_io.cpp
    unit/celestial_sphere.cpp
    unit/checked_math.cpp
    unit/codecs.cpp
    unit/color.cpp
    unit/compressed_block.cpp
    unit/container.cpp
    unit/data_block.cpp
    unit/data_encoding.cpp
    unit/diagnostic_log.cpp
    unit/display.cpp
    unit/distortion_model.cpp
    unit/endian.cpp
    unit/error.cpp
    unit/file_io.cpp
    unit/file_resolver.cpp
    unit/format.cpp
    unit/format_specifier.cpp
    unit/hash.cpp
    unit/header_tree.cpp
    unit/held_size.cpp
    unit/image.cpp
    unit/image_attributes.cpp
    unit/io_interfaces.cpp
    unit/limits.cpp
    unit/memory_io.cpp
    unit/namespaces.cpp
    unit/orientation.cpp
    unit/outline.cpp
    unit/paths.cpp
    unit/pixel_layout.cpp
    unit/pixels.cpp
    unit/property_catalog.cpp
    unit/property_data.cpp
    unit/property_text.cpp
    unit/property_types.cpp
    unit/property_value.cpp
    unit/quote.cpp
    unit/reader.cpp
    unit/shuffle.cpp
    unit/stream_io.cpp
    unit/text_grammar.cpp
    unit/thread_safe_source.cpp
    unit/utc_time.cpp
    unit/utf16.cpp
    unit/utf8.cpp
    unit/uuid.cpp
    unit/version.cpp
    unit/xml_document.cpp
    unit/xml_writer.cpp
    unit/xoshiro.cpp)

# Calls of the library from the oneTBB tasks of an application.
if(OPENXISF_WITH_TBB)
    target_sources(openxisf_tests PRIVATE concurrency/tbb.cpp)
endif()

# The tests that count the memory that code asks for, or make its allocations fail, through an operator new that their
# executable puts in place of the standard library's for every test that it runs. In openxisf_tests it would take from
# the whole suite the checks of a sanitizer's own allocator, such as the new-delete mismatches of AddressSanitizer.
# ThreadSanitizer defines operator new in a runtime library that Clang links whole, so that no executable can replace
# it: a build with ThreadSanitizer has no allocation tests.
if("thread" IN_LIST OPENXISF_SANITIZE)
    set(allocation_tests OFF)
else()
    set(allocation_tests ON)
endif()
if(allocation_tests)
    openxisf_add_test_executable(openxisf_allocation_tests
        allocation/allocations.cpp
        allocation/block_attributes.cpp
        allocation/compressed_block.cpp
        allocation/data_block.cpp
        allocation/property.cpp
        allocation/reader.cpp
        allocation/writer.cpp
        support/fixture_builder.cpp)
endif()

# The suites of concurrency/, conformance/ and samples/ are named after their directory, which gives them their label.
# The name of a parameterized suite follows the name of its instantiation and a slash.
gtest_discover_tests(openxisf_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "-concurrency_*:*/concurrency_*:conformance_*:*/conformance_*:samples_*:*/samples_*"
    PROPERTIES LABELS unit)
foreach(label concurrency conformance samples)
    gtest_discover_tests(openxisf_tests
        DISCOVERY_MODE PRE_TEST
        TEST_FILTER "${label}_*:*/${label}_*"
        PROPERTIES LABELS ${label})
endforeach()
# The tests of memory are unit tests.
if(allocation_tests)
    gtest_discover_tests(openxisf_allocation_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS unit)
endif()

# The tests that take gigabytes of memory, and the large samples, also have the label slow. CTest knows them only once it
# has discovered them, and gtest_discover_tests() splits a list of labels into arguments, so a file that CTest includes
# after the discovered tests gives them both labels.
set(slow_tests_file "${CMAKE_CURRENT_BINARY_DIR}/slow_tests.cmake")
file(WRITE "${slow_tests_file}"
    "set_tests_properties(file_source.reads_more_than_1_gib_in_one_call\n"
    "    hash.hashes_a_message_of_more_than_4_gib_in_one_piece PROPERTIES LABELS \"unit;slow\")\n"
    "set_tests_properties(samples_large.l1_decompresses_with_the_subblocks_of_pixinsight\n"
    "    PROPERTIES LABELS \"samples;slow\")\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${slow_tests_file}")

# The installed package: install into an empty staging prefix, then configure, build and run tests/package against it.
# The five tests are a chain, which a static library follows with a test of its own; fixtures make a failed step skip
# the following ones. With a multi-configuration generator, each step takes the configuration of the CTest run (ctest
# -C). The build of the consumer is configured again over the previous one: on Windows its directory can stay in use
# for minutes after a build.
if(OPENXISF_INSTALL)
    set(package_root "${CMAKE_CURRENT_BINARY_DIR}/package")
    set(package_stage "${package_root}/stage")
    set(package_build "${package_root}/build")

    # The consumer must be built like the library: same generator, compiler, flags and dependencies, the staged package
    # first. A list value is passed whole, with $<SEMICOLON>, which add_test() would otherwise split into arguments.
    set(prefix_path "${package_stage}" ${CMAKE_PREFIX_PATH})
    list(JOIN prefix_path "$<SEMICOLON>" prefix_path)
    set(package_args "-DCMAKE_PREFIX_PATH=${prefix_path}" "-DOPENXISF_PACKAGE_VERSION=${PROJECT_VERSION}")
    foreach(variable
            CMAKE_BUILD_TYPE CMAKE_CXX_COMPILER CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS
            CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_MSVC_RUNTIME_LIBRARY
            CMAKE_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_CHAINLOAD_TOOLCHAIN_FILE VCPKG_OVERLAY_TRIPLETS)
        if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
            string(REPLACE ";" "$<SEMICOLON>" value "${${variable}}")
            list(APPEND package_args "-D${variable}=${value}")
        endif()
    endforeach()
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND package_args -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND package_args -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()

    # A multi-configuration consumer is generated for the configuration of the test alone, the only one installed: the
    # toolchain of vcpkg maps RelWithDebInfo and MinSizeRel to Release, which a stage of Debug lacks.
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        list(APPEND package_args "-DCMAKE_CONFIGURATION_TYPES=$<CONFIG>")
        set(config_args --config "$<CONFIG>")
        set(consumer "${package_build}/$<CONFIG>/consumer${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        set(config_args)
        set(consumer "${package_build}/consumer${CMAKE_EXECUTABLE_SUFFIX}")
    endif()

    set(package_path "${package_stage}/${CMAKE_INSTALL_BINDIR}")
    if(DEFINED VCPKG_TARGET_TRIPLET)
        # The consumer uses the packages that the library was built with, not a global vcpkg tree.
        list(APPEND package_args "-DVCPKG_MANIFEST_MODE=OFF" "-DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}")
        # Windows finds the DLLs of a shared build through PATH; vcpkg keeps those of Debug apart.
        list(APPEND package_path "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin")
    endif()

    add_test(NAME package.clean
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package_stage}")
    add_test(NAME package.install
        COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" ${config_args} --prefix "${package_stage}")
    add_test(NAME package.configure
        COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/package" -B "${package_build}"
                -G "${CMAKE_GENERATOR}" ${package_args})
    add_test(NAME package.build
        COMMAND "${CMAKE_COMMAND}" --build "${package_build}" ${config_args})
    add_test(NAME package.run
        COMMAND "${consumer}")

    set_tests_properties(package.clean PROPERTIES FIXTURES_SETUP package_cleaned)
    set_tests_properties(package.install PROPERTIES
        FIXTURES_REQUIRED package_cleaned FIXTURES_SETUP package_installed)
    set_tests_properties(package.configure PROPERTIES
        FIXTURES_REQUIRED package_installed FIXTURES_SETUP package_configured)
    set_tests_properties(package.build PROPERTIES
        FIXTURES_REQUIRED package_configured FIXTURES_SETUP package_built)
    set_tests_properties(package.run PROPERTIES FIXTURES_REQUIRED package_built)

    set_tests_properties(package.clean package.install package.configure package.build package.run
        PROPERTIES LABELS package)
    foreach(directory ${package_path})
        set_property(TEST package.run APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${directory}")
    endforeach()

    # A static package finds ZLIB, lz4 and zstd through their modules, also for a consumer that prefers CMake packages
    # (CMAKE_FIND_PACKAGE_PREFER_CONFIG, which Conan sets), where those of package/prefer_config come first, whose
    # targets have other names. Only the configuration is checked, in a build directory of its own.
    if(NOT OPENXISF_SHARED)
        set(stand_ins "${CMAKE_CURRENT_SOURCE_DIR}/package/prefer_config")
        add_test(NAME package.prefer_config
            COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/package" -B "${package_root}/prefer_config"
                    -G "${CMAKE_GENERATOR}" ${package_args} -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
                    "-DZLIB_DIR=${stand_ins}" "-Dlz4_DIR=${stand_ins}" "-Dzstd_DIR=${stand_ins}")
        set_tests_properties(package.prefer_config PROPERTIES FIXTURES_REQUIRED package_installed LABELS package)
    endif()
endif()
