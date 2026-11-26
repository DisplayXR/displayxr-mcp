# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# ctest `version` (#41): the version the build reports must be what
# `git describe` says about the source tree right now, so a stale in-tree
# number can never ship again. Skipped (prints "SKIP:", matched by the
# test's SKIP_REGULAR_EXPRESSION) when the sources are not a git checkout
# with a reachable vX.Y.Z tag (e.g. a shallow clone).
#
#   cmake -DSRC=<source dir> -DEXPECTED=<configured version> -P check_version.cmake

if(NOT EXISTS "${SRC}/.git")
    message("SKIP: ${SRC} is not a git checkout")
    return()
endif()
find_program(GIT NAMES git)
if(NOT GIT)
    message("SKIP: git not found")
    return()
endif()
execute_process(
    COMMAND "${GIT}" -C "${SRC}" describe --tags --match "v[0-9]*" --abbrev=7
    OUTPUT_VARIABLE desc
    RESULT_VARIABLE rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT rc EQUAL 0 OR NOT desc MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9]+-g[0-9a-f]+)?)$")
    message("SKIP: no reachable vX.Y.Z tag")
    return()
endif()
set(want "${CMAKE_MATCH_1}")
if(NOT EXPECTED STREQUAL want)
    message(FATAL_ERROR "build reports version '${EXPECTED}' but git describe says '${want}' (reconfigure, or check cmake/DisplayXRMCPVersion.cmake)")
endif()
message("version ${EXPECTED} matches git describe")
