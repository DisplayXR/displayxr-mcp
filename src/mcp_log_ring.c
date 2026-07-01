// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Lock-protected MPSC log ring used by the @c tail_log tool.
 *
 * The framework no longer hooks into a global logging system on its own
 * — embedders push log lines via @ref mcp_log_ring_append. This keeps
 * the library independent of the runtime's u_log_set_sink ABI.
 */

#include "displayxr_mcp/mcp_log_ring.h"
#include "displayxr_mcp/os_compat.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RING_SIZE 512 /* must be power of two */
#if (RING_SIZE & (RING_SIZE - 1)) != 0
#error "RING_SIZE must be a power of two"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mcp_log_entry g_ring[RING_SIZE];
static uint64_t g_next_seq = 1;
static bool g_started = false;

void
mcp_log_ring_start(void)
{
	pthread_mutex_lock(&g_lock);
	g_started = true;
	pthread_mutex_unlock(&g_lock);
}

void
mcp_log_ring_stop(void)
{
	pthread_mutex_lock(&g_lock);
	g_started = false;
	pthread_mutex_unlock(&g_lock);
}

void
mcp_log_ring_append(enum mcp_log_level level, const char *fmt, va_list args)
{
	char text[MCP_LOG_MAX_TEXT];
	int n = vsnprintf(text, sizeof(text), fmt, args);
	if (n < 0) {
		return;
	}
	mcp_log_ring_append_text(level, text);
}

void
mcp_log_ring_append_text(enum mcp_log_level level, const char *text)
{
	if (text == NULL) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	if (!g_started) {
		pthread_mutex_unlock(&g_lock);
		return;
	}
	uint64_t seq = g_next_seq++;
	struct mcp_log_entry *e = &g_ring[seq & (RING_SIZE - 1)];
	e->seq = seq;
	e->timestamp_ns = (int64_t)mcp_monotonic_ns();
	e->level = level;
	size_t n = strlen(text);
	size_t copy = n < sizeof(e->text) ? n : sizeof(e->text) - 1;
	memcpy(e->text, text, copy);
	e->text[copy] = '\0';
	pthread_mutex_unlock(&g_lock);
}

void
mcp_log_ring_read(uint64_t since,
                  struct mcp_log_entry *out_entries,
                  size_t max_entries,
                  size_t *out_count,
                  uint64_t *out_next_cursor,
                  uint64_t *out_dropped)
{
	pthread_mutex_lock(&g_lock);
	uint64_t next = g_next_seq;
	uint64_t oldest_in_ring = (next > RING_SIZE) ? (next - RING_SIZE) : 1;
	uint64_t start = since + 1;
	uint64_t dropped = 0;
	if (start < oldest_in_ring) {
		dropped = oldest_in_ring - start;
		start = oldest_in_ring;
	}
	size_t count = 0;
	while (start < next && count < max_entries) {
		out_entries[count++] = g_ring[start & (RING_SIZE - 1)];
		start++;
	}
	pthread_mutex_unlock(&g_lock);
	*out_count = count;
	*out_next_cursor = start > 0 ? start - 1 : 0;
	*out_dropped = dropped;
}
