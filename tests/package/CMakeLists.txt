# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# A consumer of OpenXISF, which finds the library as OPENXISF_CONSUMED_AS says: the installed package through
# find_package (the default), the installed pkg-config file, or the sources of OPENXISF_SOURCE_DIR through FetchContent,
# as a project that is not the top level. The test suite of the main project builds it against a staged install (see
# tests/CMakeLists.txt), and CI builds it against the vcpkg port, through pkg-config and from the sources. Given
# OPENXISF_PACKAGE_VERSION, the version of the package, it also checks the versions that the package accepts.

cmake_minimum_required(VERSION 3.25)

project(openxisf_package_test LANGUAGES CXX)

set(OPENXISF_CONSUMED_AS "package" CACHE STRING "How the consumer finds OpenXISF: package, pkg-config or sources")
set(OPENXISF_SOURCE_DIR "" CACHE PATH "The sources of OpenXISF, for OPENXISF_CONSUMED_AS=sources")
set(OPENXISF_PACKAGE_VERSION "" CACHE STRING "The version of the package, for OPENXISF_CONSUMED_AS=package")

add_executable(consumer main.cpp)

if(OPENXISF_CONSUMED_AS STREQUAL "package" AND OPENXISF_PACKAGE_VERSION)
    # A package of version M.m.p, M of 1 or more, is found for a request of M.0 to M.m, and not for one of a later
    # minor version or of an earlier major version (SameMajorVersion).
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" major_minor "${OPENXISF_PACKAGE_VERSION}")
    set(major "${CMAKE_MATCH_1}")
    set(minor "${CMAKE_MATCH_2}")
    math(EXPR later_minor "${minor} + 1")
    math(EXPR earlier_major "${major} - 1")
    find_package(openxisf ${major}.0 CONFIG REQUIRED)
    set(version "${openxisf_VERSION}")
    foreach(request ${major}.${later_minor} ${earlier_major}.0)
        find_package(openxisf ${request} CONFIG QUIET)
        if(openxisf_FOUND)
            message(FATAL_ERROR "openxisf ${version} was found for version ${request}")
        endif()
    endforeach()
    find_package(openxisf ${major_minor} CONFIG REQUIRED)
    target_link_libraries(consumer PRIVATE openxisf::openxisf)
elseif(OPENXISF_CONSUMED_AS STREQUAL "package")
    find_package(openxisf CONFIG REQUIRED)
    target_link_libraries(consumer PRIVATE openxisf::openxisf)
    set(version "${openxisf_VERSION}")
elseif(OPENXISF_CONSUMED_AS STREQUAL "pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(openxisf REQUIRED openxisf)
    # The static flags add the libraries of the private requirements, which a static library needs and a shared one
    # does not list. Libraries are linked after the objects that need them.
    target_compile_options(consumer PRIVATE ${openxisf_STATIC_CFLAGS})
    target_link_libraries(consumer PRIVATE ${openxisf_STATIC_LDFLAGS})
    target_compile_features(consumer PRIVATE cxx_std_20)
    set(version "${openxisf_VERSION}")
elseif(OPENXISF_CONSUMED_AS STREQUAL "sources")
    include(FetchContent)
    FetchContent_Declare(openxisf SOURCE_DIR "${OPENXISF_SOURCE_DIR}")
    # The project that includes OpenXISF says where the programs and DLLs go: here, where the consumer is, which was
    # made before.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    FetchContent_MakeAvailable(openxisf)
    # A project that is not the top level gets the library alone, which goes where that project says.
    foreach(option
            OPENXISF_BUILD_TESTS OPENXISF_BUILD_SAMPLES OPENXISF_BUILD_BENCHMARKS OPENXISF_BUILD_FUZZERS
            OPENXISF_BUILD_DOCS OPENXISF_INSTALL)
        if(${option})
            message(FATAL_ERROR "${option} is on, in a project that is not the top level")
        endif()
    endforeach()
    get_target_property(directory openxisf RUNTIME_OUTPUT_DIRECTORY)
    if(NOT directory STREQUAL CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "The library goes to ${directory}, not to ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    target_link_libraries(consumer PRIVATE openxisf::openxisf)
    get_target_property(version openxisf VERSION)
else()
    message(FATAL_ERROR "OPENXISF_CONSUMED_AS is ${OPENXISF_CONSUMED_AS}, not package, pkg-config or sources")
endif()

target_compile_definitions(consumer PRIVATE EXPECTED_VERSION="${version}")
