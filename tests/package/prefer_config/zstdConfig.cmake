# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# A CMake package of zstd whose only target is zstd::libzstd_static, without zstd::libzstd, for the test
# package.prefer_config (see tests/CMakeLists.txt). It links nothing: the test only configures.

if(NOT TARGET zstd::libzstd_static)
    add_library(zstd::libzstd_static INTERFACE IMPORTED)
endif()
