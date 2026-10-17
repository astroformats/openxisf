# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

# Replays the seeds of a fuzz target from a copy in DIRECTORY, whose name holds characters of several scripts, which no
# ANSI code page of Windows holds together, so that the replay driver must take its paths in UTF-16 there.
# Usage: cmake -D TARGET=<replay program> -D SEEDS=<directory> -D DIRECTORY=<directory> -P replay_path.cmake

file(REMOVE_RECURSE "${DIRECTORY}")
file(COPY "${SEEDS}/" DESTINATION "${DIRECTORY}")
execute_process(COMMAND "${TARGET}" "${DIRECTORY}" RESULT_VARIABLE result)
file(REMOVE_RECURSE "${DIRECTORY}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the replay of the seeds in ${DIRECTORY} failed: ${result}")
endif()
