# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

find_package(benchmark CONFIG REQUIRED)

add_executable(openxisf_benchmarks main.cpp codecs.cpp units.cpp)
target_link_libraries(openxisf_benchmarks PRIVATE openxisf_internal benchmark::benchmark)
set_target_properties(openxisf_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
openxisf_enable_warnings(openxisf_benchmarks)
openxisf_enable_sanitizers(openxisf_benchmarks)
if(OPENXISF_HARDENING)
    openxisf_enable_hardening(openxisf_benchmarks)
endif()

# One short run of each benchmark, so that they keep working. Its numbers mean nothing; run the program itself, in a
# release build, for real ones. The benchmarks of the frame/ family work on about 100 MiB, so the smoke run leaves them
# out.
add_test(NAME benchmarks.smoke
    COMMAND openxisf_benchmarks --benchmark_min_time=0.001s "--benchmark_filter=-^frame/")
set_tests_properties(benchmarks.smoke PROPERTIES LABELS benchmark)
