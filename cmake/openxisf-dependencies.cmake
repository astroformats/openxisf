# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# lz4 and zstd, the private dependencies of a static library that come with find modules, for openxisf-config.cmake,
# which includes this file with the modules in the module path. They are found by the modules, which take the CMake
# packages of lz4 and zstd too, even when CMAKE_FIND_PACKAGE_PREFER_CONFIG would find such a package first, whose
# targets have other names. find_dependency() returns from this file when a package is missing, so that the
# configuration file restores the module path before it stops.

find_dependency(lz4 MODULE)
find_dependency(zstd MODULE)
set(openxisf_dependencies_found TRUE)
