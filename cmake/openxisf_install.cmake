# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Install rules and the CMake and pkg-config packages. Included by the top-level project.

include(CMakePackageConfigHelpers)

set(OPENXISF_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/openxisf")

install(TARGETS openxisf
    EXPORT openxisf_targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/openxisf" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PROJECT_BINARY_DIR}/include/openxisf/version.h" "${PROJECT_BINARY_DIR}/include/openxisf/linkage.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/openxisf")

if(MSVC AND OPENXISF_SHARED)
    install(FILES "$<TARGET_PDB_FILE:openxisf>" DESTINATION "${CMAKE_INSTALL_BINDIR}" OPTIONAL)
endif()

install(EXPORT openxisf_targets
    NAMESPACE openxisf::
    FILE openxisf-targets.cmake
    DESTINATION "${OPENXISF_CONFIG_DIR}")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/openxisf-config.cmake.in"
    "${PROJECT_BINARY_DIR}/openxisf-config.cmake"
    INSTALL_DESTINATION "${OPENXISF_CONFIG_DIR}")
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/openxisf-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ${OPENXISF_VERSION_COMPATIBILITY})
install(FILES
    "${PROJECT_BINARY_DIR}/openxisf-config.cmake"
    "${PROJECT_BINARY_DIR}/openxisf-config-version.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/openxisf-dependencies.cmake"
    DESTINATION "${OPENXISF_CONFIG_DIR}")
install(FILES
    "${PROJECT_SOURCE_DIR}/cmake/Findlz4.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/Findzstd.cmake"
    DESTINATION "${OPENXISF_CONFIG_DIR}/modules")

# pkg-config. The prefix is relative to the file, so the installed tree can be moved. Any absolute
# directory will do as the root of the computation. GNUInstallDirs also allows absolute directories, which
# are written as they are; when the library directory, where the file goes, is one, the prefix is that of
# the installation, which cmake --install --prefix can change after the configuration: the file gets it
# when it is installed.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(OPENXISF_PC_PREFIX "@OPENXISF_PC_INSTALL_PREFIX@")
else()
    file(RELATIVE_PATH relative_prefix
        "${PROJECT_BINARY_DIR}/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${PROJECT_BINARY_DIR}/prefix")
    string(REGEX REPLACE "/$" "" relative_prefix "${relative_prefix}")
    set(OPENXISF_PC_PREFIX "\${pcfiledir}/${relative_prefix}")
endif()
foreach(directory LIBDIR INCLUDEDIR)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_${directory}}")
        set(OPENXISF_PC_${directory} "${CMAKE_INSTALL_${directory}}")
    else()
        set(OPENXISF_PC_${directory} "\${prefix}/${CMAKE_INSTALL_${directory}}")
    endif()
endforeach()
set(OPENXISF_PC_REQUIRES_PRIVATE "")
if(NOT OPENXISF_SHARED)
    set(requires pugixml zlib liblz4 libzstd)
    if(OPENXISF_WITH_TBB)
        list(APPEND requires tbb)
    endif()
    if(OPENXISF_WITH_OPENSSL)
        list(APPEND requires libcrypto)
    endif()
    list(JOIN requires ", " requires_list)
    set(OPENXISF_PC_REQUIRES_PRIVATE "Requires.private: ${requires_list}")
endif()
# The file names the library of the configuration that is installed, whose name ends with a d in a Debug build, so it is
# generated for each configuration, which a multi-configuration generator knows only then. pkg-config has no
# configurations: installed into one prefix, they share the file, which describes the last one installed.
set(OPENXISF_PC_LIBRARY "$<TARGET_LINKER_FILE_BASE_NAME:openxisf>")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/openxisf.pc.in"
    "${PROJECT_BINARY_DIR}/openxisf.pc.genex"
    @ONLY)
set(pc_file "openxisf$<$<CONFIG:Debug>:-debug>.pc")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/${pc_file}" INPUT "${PROJECT_BINARY_DIR}/openxisf.pc.genex")
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    install(CODE "
        set(OPENXISF_PC_INSTALL_PREFIX \"\${CMAKE_INSTALL_PREFIX}\")
        configure_file(\"${PROJECT_BINARY_DIR}/${pc_file}\" \"${PROJECT_BINARY_DIR}/installed/${pc_file}\" @ONLY)")
    install(FILES "${PROJECT_BINARY_DIR}/installed/${pc_file}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        RENAME openxisf.pc)
else()
    install(FILES "${PROJECT_BINARY_DIR}/${pc_file}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        RENAME openxisf.pc)
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
