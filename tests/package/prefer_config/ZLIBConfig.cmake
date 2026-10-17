# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# A CMake package of zlib whose only target is ZLIB::ZLIBSTATIC, without ZLIB::ZLIB, as the package of zlib 1.3.2 is
# for a static library, for the test package.prefer_config (see tests/CMakeLists.txt). It links nothing: the test only
# configures.

if(NOT TARGET ZLIB::ZLIBSTATIC)
    add_library(ZLIB::ZLIBSTATIC INTERFACE IMPORTED)
endif()
