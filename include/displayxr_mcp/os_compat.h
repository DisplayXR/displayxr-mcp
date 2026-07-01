// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Tiny stand-ins for the runtime aux-util surface this library
 *         used to depend on. Lets displayxr_mcp build with no DisplayXR
 *         runtime headers — only cJSON, pthreads, and the C runtime.
 *
 * Originally u_mcp_* in the runtime relied on @c U_LOG_W / @c U_LOG_I
 * for bring-up messages, @c U_TYPED_CALLOC for typed allocation, and
 * @c os_monotonic_get_ns for cross-platform monotonic time. That tied
 * the framework to the runtime's aux layer. We restate the same surface
 * here in ~50 lines so the framework is self-contained.
 *
 * Logging policy: bring-up messages go to stderr by default. Embedders
 * who want them routed elsewhere set @ref mcp_set_log_fn at process
 * start. (This is for the framework's own diagnostic output — runtime
 * logs that the @c tail_log tool exposes go through @c mcp_log_ring.)
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- Typed allocation (replaces aux/util/u_misc.h's U_TYPED_CALLOC) ----------

#define MCP_TYPED_CALLOC(T) ((T *)calloc(1, sizeof(T)))

// ---------- Monotonic clock (replaces aux/os/os_time.h's os_monotonic_get_ns) ----------

uint64_t
mcp_monotonic_ns(void);

// ---------- Internal logging (replaces aux/util/u_logging.h's U_LOG_*) ----------
//
// These are framework bring-up diagnostics ("listening on /tmp/foo",
// "tool registry full"). They are NOT the log lines the tail_log tool
// returns — those go through mcp_log_ring.

enum mcp_internal_log_level
{
	MCP_INTERNAL_LOG_INFO = 0,
	MCP_INTERNAL_LOG_WARN = 1,
	MCP_INTERNAL_LOG_ERROR = 2,
};

typedef void (*mcp_internal_log_fn)(enum mcp_internal_log_level level,
                                    const char *fmt,
                                    va_list args);

/*!
 * Install a process-wide callback for framework bring-up logs. Pass NULL
 * to revert to the default (stderr). Idempotent. Not thread-safe with
 * concurrent calls to MCP_LOG_* but safe to call once at process start.
 */
void
mcp_set_log_fn(mcp_internal_log_fn fn);

void
mcp_internal_log(enum mcp_internal_log_level level, const char *fmt, ...);

#define MCP_LOG_I(...) mcp_internal_log(MCP_INTERNAL_LOG_INFO, __VA_ARGS__)
#define MCP_LOG_W(...) mcp_internal_log(MCP_INTERNAL_LOG_WARN, __VA_ARGS__)
#define MCP_LOG_E(...) mcp_internal_log(MCP_INTERNAL_LOG_ERROR, __VA_ARGS__)
// MCP_LOG_D / MCP_LOG_T are accepted for source compatibility — they
// route to INFO. The framework rarely emits debug/trace itself.
#define MCP_LOG_D(...) mcp_internal_log(MCP_INTERNAL_LOG_INFO, __VA_ARGS__)
#define MCP_LOG_T(...) mcp_internal_log(MCP_INTERNAL_LOG_INFO, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
