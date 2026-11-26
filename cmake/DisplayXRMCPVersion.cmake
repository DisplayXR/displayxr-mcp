# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# One source for the displayxr-mcp version: the release tag (vX.Y.Z).
#
# Included BEFORE project() so project(VERSION) — and everything derived
# from it (serverInfo.version of the in-process server and the workspace
# aggregator, installer file names, Capabilities\MCP\Version) — reads the
# same value. The version used to be a literal in project() that nobody
# bumped at release time, so every consumer that builds this repo from a
# tag (FetchContent in the runtime and the shell) reported a stale one.
#
# Resolution order:
#   1. A git checkout of THIS repo (a clone, or FetchContent's clone in
#      _deps/): `git describe --tags --match 'v[0-9]*'`. On a tag that is
#      "X.Y.Z"; past a tag it is "X.Y.Z-N-g<sha>", so a dev build is never
#      mistaken for the release.
#   2. A `git archive` export (GitHub's "Source code" downloads):
#      cmake/version-archive.txt, which git expands at export time
#      (export-subst in .gitattributes) to the ref names of the commit,
#      e.g. "tag: v0.5.8".
#   3. A plain copy of the sources with no git metadata at all: "0.0.0",
#      reported as "0.0.0-unknown". Deliberately not a real release number:
#      an in-tree number is exactly what went stale before, and an honest
#      "unknown" beats a wrong version.
#
# Outputs:
#   DISPLAYXR_MCP_VERSION_NUMERIC  X.Y.Z, for project(VERSION).
#   DISPLAYXR_MCP_VERSION_STRING   what the server reports (adds -N-g<sha>).
#   DISPLAYXR_MCP_VERSION_SOURCE   where it came from (for the status line).

set(_dxrmcp_ver "")
set(_dxrmcp_src "")
set(_dxrmcp_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_dxrmcp_root "${_dxrmcp_root}" REALPATH)

# 1. git describe — only if the git top level IS this source tree, so a
# copy vendored inside another repository never picks up that repo's tags.
find_program(DISPLAYXR_MCP_GIT_EXECUTABLE NAMES git)
mark_as_advanced(DISPLAYXR_MCP_GIT_EXECUTABLE)
if(DISPLAYXR_MCP_GIT_EXECUTABLE)
    execute_process(
        COMMAND "${DISPLAYXR_MCP_GIT_EXECUTABLE}" -C "${_dxrmcp_root}" rev-parse --show-toplevel
        OUTPUT_VARIABLE _dxrmcp_top
        RESULT_VARIABLE _dxrmcp_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    set(_dxrmcp_here "${_dxrmcp_root}")
    if(_dxrmcp_rc EQUAL 0)
        get_filename_component(_dxrmcp_top "${_dxrmcp_top}" REALPATH)
        if(WIN32 OR CMAKE_HOST_WIN32)
            # Drive-letter case can differ between git and CMake.
            string(TOLOWER "${_dxrmcp_top}" _dxrmcp_top)
            string(TOLOWER "${_dxrmcp_here}" _dxrmcp_here)
        endif()
    endif()
    if(_dxrmcp_rc EQUAL 0 AND _dxrmcp_top STREQUAL _dxrmcp_here)
        execute_process(
            COMMAND "${DISPLAYXR_MCP_GIT_EXECUTABLE}" -C "${_dxrmcp_root}"
                    describe --tags --match "v[0-9]*" --abbrev=7
            OUTPUT_VARIABLE _dxrmcp_desc
            RESULT_VARIABLE _dxrmcp_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_dxrmcp_rc EQUAL 0 AND _dxrmcp_desc MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9]+-g[0-9a-f]+)?$")
            set(_dxrmcp_ver "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
            set(_dxrmcp_src "git")
        endif()
        # Re-run configure when HEAD moves (checkout, commit, new tag):
        # HEAD itself, the branch ref it points at, and packed-refs.
        set(_dxrmcp_deps HEAD packed-refs)
        if(EXISTS "${_dxrmcp_root}/.git/HEAD")
            file(STRINGS "${_dxrmcp_root}/.git/HEAD" _dxrmcp_head LIMIT_COUNT 1)
            if(_dxrmcp_head MATCHES "^ref: (.+)$")
                list(APPEND _dxrmcp_deps "${CMAKE_MATCH_1}")
            endif()
        endif()
        foreach(_f IN LISTS _dxrmcp_deps)
            if(EXISTS "${_dxrmcp_root}/.git/${_f}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dxrmcp_root}/.git/${_f}")
            endif()
        endforeach()
    endif()
endif()

# 2. git archive export: "$Format:%D$" expanded to e.g. "HEAD -> main, tag: v0.5.8".
if(_dxrmcp_ver STREQUAL "" AND EXISTS "${_dxrmcp_root}/cmake/version-archive.txt")
    file(READ "${_dxrmcp_root}/cmake/version-archive.txt" _dxrmcp_refs)
    # Trailing "," so a tag at the very end still has a delimiter.
    if("${_dxrmcp_refs}," MATCHES "tag: v([0-9]+\\.[0-9]+\\.[0-9]+)[,\n\r ]")
        set(_dxrmcp_ver "${CMAKE_MATCH_1}")
        set(_dxrmcp_src "archive")
    endif()
endif()

# 3. Plain source copy: no version information at all.
if(_dxrmcp_ver STREQUAL "")
    set(_dxrmcp_ver "0.0.0-unknown")
    set(_dxrmcp_src "no git metadata")
    message(WARNING "displayxr_mcp: no git tag or archive metadata found; "
                    "the server will report version ${_dxrmcp_ver}")
endif()

string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" DISPLAYXR_MCP_VERSION_NUMERIC "${_dxrmcp_ver}")
set(DISPLAYXR_MCP_VERSION_STRING "${_dxrmcp_ver}")
set(DISPLAYXR_MCP_VERSION_SOURCE "${_dxrmcp_src}")
message(STATUS "displayxr_mcp version ${DISPLAYXR_MCP_VERSION_STRING} (from ${DISPLAYXR_MCP_VERSION_SOURCE})")
