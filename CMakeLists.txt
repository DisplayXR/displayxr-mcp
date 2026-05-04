# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)

project(displayxr_mcp
    VERSION 0.2.3
    DESCRIPTION "Tiny embeddable MCP (Model Context Protocol) server framework"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

option(DISPLAYXR_MCP_BUILD_ADAPTER "Build the displayxr-mcp stdio↔socket adapter binary" ON)
option(DISPLAYXR_MCP_INSTALL "Install the lib + headers + adapter" ON)
option(DISPLAYXR_MCP_BUILD_TESTS "Build the framework smoke test (ctest target)" OFF)

# ---------------------------------------------------------------- deps

# cJSON: JSON parsing for the JSON-RPC dispatcher. Detection chain:
#   1. Parent project already has a cJSON::cJSON or cjson target → reuse.
#   2. find_package(cJSON CONFIG) succeeds → use it.
#   3. Fall back to the vendored copy in external/cjson/ — keeps the
#      framework self-contained on hosts without a system cJSON.
#
# The vendored copy is the upstream cJSON with CJSON_HIDE_SYMBOLS so it
# never collides at link time with another consumer's cJSON.
option(DISPLAYXR_MCP_USE_VENDORED_CJSON "Force use of the vendored cJSON copy under external/cjson/" OFF)

if(NOT DISPLAYXR_MCP_USE_VENDORED_CJSON)
    if(TARGET cJSON::cJSON)
        set(DISPLAYXR_MCP_CJSON_TARGET cJSON::cJSON)
    elseif(TARGET cjson)
        set(DISPLAYXR_MCP_CJSON_TARGET cjson)
    else()
        find_package(cJSON CONFIG QUIET)
        if(TARGET cJSON::cJSON)
            set(DISPLAYXR_MCP_CJSON_TARGET cJSON::cJSON)
        elseif(TARGET cjson)
            set(DISPLAYXR_MCP_CJSON_TARGET cjson)
        endif()
    endif()
endif()

if(NOT DEFINED DISPLAYXR_MCP_CJSON_TARGET)
    # Vendored fallback — compile cJSON.c into a small static lib with
    # symbols hidden so we don't conflict with any other cJSON the
    # parent (or any sibling sub-project) may ship.
    add_library(displayxr_mcp_vendored_cjson STATIC external/cjson/cJSON.c)
    target_include_directories(displayxr_mcp_vendored_cjson SYSTEM
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external>)
    target_compile_definitions(displayxr_mcp_vendored_cjson PUBLIC CJSON_HIDE_SYMBOLS)
    set_target_properties(displayxr_mcp_vendored_cjson PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)
    set(DISPLAYXR_MCP_CJSON_TARGET displayxr_mcp_vendored_cjson)
endif()

# Threads: pthreads on POSIX. On Windows we don't have a system pthread
# so we depend on vcpkg's `pthreads` port (declared in vcpkg.json) — it
# provides pthread.h + a static lib MSVC can link. When this lib is
# pulled into a parent project that already wired pthreads (e.g. the
# runtime's xrt-pthreads target), reuse that target instead.
set(THREADS_PREFER_PTHREAD_FLAG ON)
if(WIN32)
    if(TARGET xrt-pthreads)
        # Runtime parent project already has its pthread shim wired.
        set(DISPLAYXR_MCP_PTHREAD_TARGET xrt-pthreads)
    elseif(TARGET PThreads4W::PThreads4W)
        # vcpkg pthreads port name (modern).
        set(DISPLAYXR_MCP_PTHREAD_TARGET PThreads4W::PThreads4W)
    else()
        find_package(PThreads4W CONFIG QUIET)
        if(TARGET PThreads4W::PThreads4W)
            set(DISPLAYXR_MCP_PTHREAD_TARGET PThreads4W::PThreads4W)
        else()
            # Last resort: the legacy pthreads_windows config target.
            find_package(pthreads_windows CONFIG QUIET)
            if(TARGET PThreads4W::PThreads4W)
                set(DISPLAYXR_MCP_PTHREAD_TARGET PThreads4W::PThreads4W)
            else()
                message(FATAL_ERROR "displayxr_mcp on Windows requires vcpkg 'pthreads' (or the runtime's xrt-pthreads target).")
            endif()
        endif()
    endif()
else()
    find_package(Threads REQUIRED)
    set(DISPLAYXR_MCP_PTHREAD_TARGET Threads::Threads)
endif()

# ---------------------------------------------------------------- library

set(DISPLAYXR_MCP_SOURCES
    src/mcp_server.c
    src/mcp_transport.c
    src/mcp_capture.c
    src/mcp_audit.c
    src/mcp_allowlist.c
    src/mcp_log_ring.c
    src/os_compat.c)

add_library(displayxr_mcp STATIC ${DISPLAYXR_MCP_SOURCES})
add_library(displayxr_mcp::mcp ALIAS displayxr_mcp)

target_include_directories(displayxr_mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(displayxr_mcp
    PUBLIC ${DISPLAYXR_MCP_CJSON_TARGET} ${DISPLAYXR_MCP_PTHREAD_TARGET})

if(WIN32)
    # PIPE_PREFIX uses raw backslashes; nothing platform-specific to link
    # beyond what's already on the link line via the Windows SDK.
endif()

# ---------------------------------------------------------------- adapter

if(DISPLAYXR_MCP_BUILD_ADAPTER)
    add_executable(displayxr_mcp_adapter adapter/displayxr_mcp.c)
    add_executable(displayxr_mcp::adapter ALIAS displayxr_mcp_adapter)
    set_target_properties(displayxr_mcp_adapter PROPERTIES
        OUTPUT_NAME displayxr-mcp
        PREFIX "")
    target_link_libraries(displayxr_mcp_adapter PRIVATE displayxr_mcp)
endif()

# ---------------------------------------------------------------- tests

if(DISPLAYXR_MCP_BUILD_TESTS)
    if(NOT DISPLAYXR_MCP_BUILD_ADAPTER)
        message(FATAL_ERROR "DISPLAYXR_MCP_BUILD_TESTS requires DISPLAYXR_MCP_BUILD_ADAPTER (the smoke test forks the adapter as a subprocess).")
    endif()
    enable_testing()
    add_executable(displayxr_mcp_smoke tests/smoke_test.c)
    target_link_libraries(displayxr_mcp_smoke PRIVATE displayxr_mcp)
    add_test(NAME smoke COMMAND displayxr_mcp_smoke $<TARGET_FILE:displayxr_mcp_adapter>)
endif()

# ---------------------------------------------------------------- install

if(DISPLAYXR_MCP_INSTALL)
    install(TARGETS displayxr_mcp EXPORT displayxr_mcp_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/displayxr_mcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(DISPLAYXR_MCP_BUILD_ADAPTER)
        install(TARGETS displayxr_mcp_adapter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()
