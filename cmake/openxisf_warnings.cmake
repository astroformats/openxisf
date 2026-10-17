# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Warning, language and hardening flags for the targets of this project. Every flag is PRIVATE, so
# nothing is imposed on consumers of the library.

function(openxisf_enable_warnings target)
    set(flags)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        list(APPEND flags
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
            # Narrowing and sign conversions, virtual destructors, hidden members: off by default at /W4.
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14826 /w14905 /w14906 /w14928)
        if(OPENXISF_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
    elseif(MSVC)
        # clang-cl maps -Wall to /Wall, which is -Weverything, so it takes /W4 (= -Wall -Wextra) instead. -Wdeprecated, as
        # the public headers must pass it for consumers that include them without SYSTEM.
        list(APPEND flags
            /W4 -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align
            -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wdeprecated)
        if(OPENXISF_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
    else()
        # GCC, Clang and AppleClang. The deprecations, as the public headers must pass them for consumers that include
        # them without SYSTEM: GCC enables -Wdeprecated by default, but not the copy of a class with a destructor.
        list(APPEND flags
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align
            -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND flags -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast -Wdeprecated-copy-dtor)
        else()
            list(APPEND flags -Wdeprecated)
        endif()
        if(OPENXISF_WARNINGS_AS_ERRORS)
            list(APPEND flags -Werror)
        endif()
    endif()

    target_compile_options(${target} PRIVATE ${flags})
endfunction()

# Hardening for CI and release builds (OPENXISF_HARDENING). Never applied to consumers.
function(openxisf_enable_hardening target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /GS /sdl /guard:cf)
        target_link_options(${target} PRIVATE /GUARD:CF)
        return()
    endif()

    if(MSVC)
        # clang-cl.
        target_compile_options(${target} PRIVATE /guard:cf)
        target_link_options(${target} PRIVATE /GUARD:CF)
        return()
    endif()

    # Standard library checks: libstdc++ assertions, and libc++ hardening (ignored by libc++ older than 18).
    target_compile_definitions(${target} PRIVATE
        _GLIBCXX_ASSERTIONS
        _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)

    if(NOT WIN32)
        # MinGW would also need libssp on the link line.
        target_compile_options(${target} PRIVATE -fstack-protector-strong)
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # _FORTIFY_SOURCE needs optimization, which glibc warns about otherwise, so only the optimized configurations
        # get it (not Debug, nor an empty CMAKE_BUILD_TYPE); distributions may already define it, hence the -U. glibc
        # grants level 3 to GCC 12 and later only, and warns that it treats it as 2 under an older GCC.
        set(fortify_level 3)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
            set(fortify_level 2)
        endif()
        set(optimized "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
        target_compile_options(${target} PRIVATE
            "$<${optimized}:-U_FORTIFY_SOURCE>"
            "$<${optimized}:-D_FORTIFY_SOURCE=${fortify_level}>")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            target_compile_options(${target} PRIVATE -fcf-protection)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            target_compile_options(${target} PRIVATE -mbranch-protection=standard)
        endif()
    endif()
endfunction()
