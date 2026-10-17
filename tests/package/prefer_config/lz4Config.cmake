# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# A CMake package of lz4 whose target is LZ4::lz4, as Conan's is, without lz4::lz4, for the test package.prefer_config
# (see tests/CMakeLists.txt). It links nothing: the test only configures.

if(NOT TARGET LZ4::lz4)
    add_library(LZ4::lz4 INTERFACE IMPORTED)
endif()
