# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# The sample programs. They use the public API only, like an application, and each one runs as a test with the label
# example. The arguments after the name are the arguments of its test.

function(openxisf_add_sample name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE openxisf::openxisf)
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
    # Windows: the UTF-8 arguments of arguments.h come from CommandLineToArgvW() of shell32, and windows.h comes without
    # the min and max macros.
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(${name} PRIVATE shell32)
    endif()
    openxisf_enable_warnings(${name})
    openxisf_enable_sanitizers(${name})
    if(OPENXISF_HARDENING)
        openxisf_enable_hardening(${name})
    endif()

    add_test(NAME example.${name} COMMAND ${name} ${ARGN})
    set_tests_properties(example.${name} PROPERTIES LABELS example)
endfunction()

set(sample_data "${PROJECT_SOURCE_DIR}/tests/data/pixinsight")

openxisf_add_sample(custom_io)
openxisf_add_sample(distributed "${CMAKE_CURRENT_BINARY_DIR}/distributed.xish")
openxisf_add_sample(preview "${sample_data}/d1-rich-rgb.xisf" "${CMAKE_CURRENT_BINARY_DIR}/preview.ppm")
openxisf_add_sample(read_info "${sample_data}/d1-rich-rgb.xisf")
# The solution of D5 too, which the test finds in the output, besides the exit code.
add_test(NAME example.read_info.astrometry
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:read_info>" "-DARGUMENT=${sample_data}/d5-astrometry.xisf"
        "-DEXPECTED=astrometric solution 1[.]0 [(]Gnomonic, distortion model layer[)], centre at RA 83[.]82"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_output.cmake")
set_tests_properties(example.read_info.astrometry PROPERTIES LABELS example)
# A String that the program cuts in a character of two bytes, the 57th and 58th, which the cut leaves out whole.
string(REPEAT "a" 56 letters)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/long_string.xish"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\">\n"
    "  <Metadata>\n"
    "    <Property id=\"XISF:CreationTime\" type=\"TimePoint\" value=\"2026-10-17T00:00:00Z\"/>\n"
    "    <Property id=\"XISF:CreatorApplication\" type=\"String\">read_info test</Property>\n"
    "  </Metadata>\n"
    "  <Property id=\"Test:Text\" type=\"String\">${letters}&#233;bbbbbbbbbb</Property>\n"
    "</xisf>\n")
add_test(NAME example.read_info.long_string
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:read_info>"
        "-DARGUMENT=${CMAKE_CURRENT_BINARY_DIR}/long_string.xish"
        "-DEXPECTED=Test:Text [(]String[)]: .${letters}[.][.][.]."
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_output.cmake")
set_tests_properties(example.read_info.long_string PROPERTIES LABELS example)
openxisf_add_sample(read_pixels "${sample_data}/a4-rgb-f32.xisf")
openxisf_add_sample(stream_io "${sample_data}/a6-rgba-u16.xisf")
openxisf_add_sample(write_image "${CMAKE_CURRENT_BINARY_DIR}/write_image.xisf")
