// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

/// @file
/// Export macro and the 64-bit check shared by every public header.

#include <openxisf/linkage.h>

// Every public header includes this one, so the 64-bit requirement is checked for every user.
static_assert(sizeof(void*) == 8, "OpenXISF supports 64-bit targets only");

/// Marks the symbols that belong to the public interface of the shared library. A static library defines
/// OPENXISF_STATIC_DEFINE in linkage.h, which its build generates.
#if defined(OPENXISF_STATIC_DEFINE)
#define OPENXISF_API
#elif defined(_WIN32)
#if defined(OPENXISF_EXPORTS)
#define OPENXISF_API __declspec(dllexport)
#else
#define OPENXISF_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPENXISF_API __attribute__((visibility("default")))
#else
#define OPENXISF_API
#endif
