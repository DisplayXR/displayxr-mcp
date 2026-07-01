// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Opt-in MPSC log ring used by the MCP @c tail_log tool.
 *
 * The framework owns the ring (fixed-size, lock-protected) and exposes
 * @ref mcp_log_ring_append for the embedder to push log lines. In the
 * runtime that means oxr_instance installs a u_log_set_sink that calls
 * @c mcp_log_ring_append for every U_LOG line; in the shell that means
 * the chrome's logging callback does the same. Either way, the ring
 * is consumer-agnostic.
 *
 * Readers (the built-in @c tail_log tool) pull by sequence cursor —
 * entries older than the reader's last-seen cursor by more than the
 * ring size are reported as dropped.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_LOG_MAX_TEXT 512

/*!
 * Severity levels the @c tail_log tool surfaces. Mapped from whatever
 * the embedder's logging system uses.
 */
enum mcp_log_level
{
	MCP_LOG_LEVEL_TRACE = 0,
	MCP_LOG_LEVEL_DEBUG = 1,
	MCP_LOG_LEVEL_INFO = 2,
	MCP_LOG_LEVEL_WARN = 3,
	MCP_LOG_LEVEL_ERROR = 4,
	MCP_LOG_LEVEL_RAW = 5,
};

struct mcp_log_entry
{
	uint64_t seq;
	int64_t timestamp_ns;
	enum mcp_log_level level;
	char text[MCP_LOG_MAX_TEXT];
};

/*!
 * Initialize the ring buffer. Idempotent. Safe to call before any push.
 */
void
mcp_log_ring_start(void);

/*!
 * Drop the ring buffer. Idempotent.
 */
void
mcp_log_ring_stop(void);

/*!
 * Push a formatted log line into the ring. Caller is responsible for
 * mapping their logging system's level to @ref mcp_log_level. Truncates
 * to @c MCP_LOG_MAX_TEXT - 1 bytes.
 */
void
mcp_log_ring_append(enum mcp_log_level level, const char *fmt, va_list args);

/*!
 * Convenience text-only variant — push a pre-formatted line.
 */
void
mcp_log_ring_append_text(enum mcp_log_level level, const char *text);

/*!
 * Read up to @p max_entries entries published with @c seq > @p since.
 * Fills @p out_entries, writes the new cursor to @p out_next_cursor,
 * and reports how many entries were lost.
 */
void
mcp_log_ring_read(uint64_t since,
                  struct mcp_log_entry *out_entries,
                  size_t max_entries,
                  size_t *out_count,
                  uint64_t *out_next_cursor,
                  uint64_t *out_dropped);

#ifdef __cplusplus
}
#endif
