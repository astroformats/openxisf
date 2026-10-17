# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Fuzz targets. When OPENXISF_SANITIZE holds "fuzzer" (Clang), each one is a libFuzzer program, and its test fuzzes for
# OPENXISF_FUZZ_SECONDS from the committed seeds in seeds/<name>. Otherwise it is linked with a replay driver, and its
# test runs the seeds once, so that the regression corpus runs on every platform. Both kinds of test have the label
# fuzz. With OPENXISF_FUZZING_ENGINE, each target is linked with that engine instead, as OSS-Fuzz builds them
# (oss-fuzz/build.sh): the instrumentation comes from CMAKE_CXX_FLAGS, and no test runs them.

set(OPENXISF_FUZZ_SECONDS 60 CACHE STRING "How long the test of each libFuzzer target fuzzes, in seconds")
set(OPENXISF_FUZZING_ENGINE "" CACHE STRING
    "Link the fuzz targets with this fuzzing engine, a flag such as -fsanitize=fuzzer or a library, with the instrumentation of CMAKE_CXX_FLAGS, as OSS-Fuzz builds them; no test runs them then")

if(OPENXISF_FUZZING_ENGINE)
    set(mode engine)
elseif("fuzzer" IN_LIST OPENXISF_SANITIZE)
    set(mode libfuzzer)
else()
    set(mode replay)
    add_library(openxisf_fuzz_replay OBJECT replay.cpp)
    set_target_properties(openxisf_fuzz_replay PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_features(openxisf_fuzz_replay PRIVATE cxx_std_20)
    openxisf_enable_warnings(openxisf_fuzz_replay)
    openxisf_enable_sanitizers(openxisf_fuzz_replay)
    if(OPENXISF_HARDENING)
        openxisf_enable_hardening(openxisf_fuzz_replay)
    endif()
    # Windows: the paths come from CommandLineToArgvW() of shell32, and windows.h comes without the min and max macros.
    if(WIN32)
        target_compile_definitions(openxisf_fuzz_replay PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(openxisf_fuzz_replay PUBLIC shell32)
    endif()
endif()

# MIN_COVERAGE is the number of edges below which a libFuzzer run counts as broken. It is set well below what the seeds
# alone reach, so that only a target that stops early falls below it.
function(openxisf_add_fuzz_target name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "MIN_COVERAGE" "SOURCES")
    string(REGEX REPLACE "^fuzz_" "" short_name "${name}")
    set(seeds "${CMAKE_CURRENT_SOURCE_DIR}/seeds/${short_name}")

    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE openxisf_internal)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    openxisf_enable_warnings(${name})
    openxisf_enable_sanitizers(${name})
    if(OPENXISF_HARDENING)
        openxisf_enable_hardening(${name})
    endif()

    if(mode STREQUAL "engine")
        target_link_libraries(${name} PRIVATE ${OPENXISF_FUZZING_ENGINE})
        return()
    endif()
    if(mode STREQUAL "libfuzzer")
        # The sanitizer flags give every target the instrumentation; this links the libFuzzer main program.
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
        add_test(NAME fuzz.${short_name}
            COMMAND "${CMAKE_COMMAND}"
                -D "FUZZER=$<TARGET_FILE:${name}>"
                -D "SEEDS=${seeds}"
                -D "CORPUS=${PROJECT_SOURCE_DIR}/fuzz/corpus/${short_name}"
                -D "ARTIFACTS=${CMAKE_CURRENT_BINARY_DIR}/artifacts/${short_name}"
                -D "SECONDS=${OPENXISF_FUZZ_SECONDS}"
                -D "MIN_COVERAGE=${arg_MIN_COVERAGE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run_fuzzer.cmake")
        # The time of the run and ten minutes more, for loading the corpus and the last input, instead of the timeout
        # of the test presets.
        math(EXPR timeout "${OPENXISF_FUZZ_SECONDS} + 600")
        set_tests_properties(fuzz.${short_name} PROPERTIES TIMEOUT ${timeout})
    else()
        target_link_libraries(${name} PRIVATE openxisf_fuzz_replay)
        add_test(NAME fuzz.${short_name} COMMAND ${name} "${seeds}")
    endif()
    set_tests_properties(fuzz.${short_name} PROPERTIES LABELS fuzz)
endfunction()

openxisf_add_fuzz_target(fuzz_text_grammar SOURCES text_grammar.cpp MIN_COVERAGE 300)
openxisf_add_fuzz_target(fuzz_header SOURCES header.cpp MIN_COVERAGE 400)
openxisf_add_fuzz_target(fuzz_block_attributes SOURCES block_attributes.cpp MIN_COVERAGE 400)
openxisf_add_fuzz_target(fuzz_codecs SOURCES codecs.cpp MIN_COVERAGE 150)
openxisf_add_fuzz_target(fuzz_property_text SOURCES property_text.cpp MIN_COVERAGE 400)
openxisf_add_fuzz_target(fuzz_roundtrip SOURCES roundtrip.cpp MIN_COVERAGE 500)
openxisf_add_fuzz_target(fuzz_xisb_index SOURCES xisb_index.cpp MIN_COVERAGE 300)
openxisf_add_fuzz_target(fuzz_astrometry SOURCES astrometry.cpp MIN_COVERAGE 300)

# The replay driver finds seeds under a path that no ANSI code page of Windows holds.
if(mode STREQUAL "replay")
    add_test(NAME fuzz.replay_path
        COMMAND "${CMAKE_COMMAND}"
            -D "TARGET=$<TARGET_FILE:fuzz_text_grammar>"
            -D "SEEDS=${CMAKE_CURRENT_SOURCE_DIR}/seeds/text_grammar"
            -D "DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/seeds Ñandú Ωμέγα 種子"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/replay_path.cmake")
    set_tests_properties(fuzz.replay_path PROPERTIES LABELS fuzz)
endif()
