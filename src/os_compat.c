// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "displayxr_mcp/os_compat.h"

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

uint64_t
mcp_monotonic_ns(void)
{
#ifdef _WIN32
	static LARGE_INTEGER s_freq;
	static int s_freq_inited = 0;
	if (!s_freq_inited) {
		QueryPerformanceFrequency(&s_freq);
		s_freq_inited = 1;
	}
	LARGE_INTEGER ctr;
	QueryPerformanceCounter(&ctr);
	// Avoid overflow on 64-bit: scale to ns by integer math.
	return (uint64_t)((double)ctr.QuadPart * 1e9 / (double)s_freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static void
default_log(enum mcp_internal_log_level level, const char *fmt, va_list args)
{
	const char *prefix = "";
	switch (level) {
	case MCP_INTERNAL_LOG_INFO: prefix = "[mcp] "; break;
	case MCP_INTERNAL_LOG_WARN: prefix = "[mcp WARN] "; break;
	case MCP_INTERNAL_LOG_ERROR: prefix = "[mcp ERROR] "; break;
	}
	fputs(prefix, stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

static mcp_internal_log_fn g_log_fn = default_log;

void
mcp_set_log_fn(mcp_internal_log_fn fn)
{
	g_log_fn = fn != NULL ? fn : default_log;
}

void
mcp_internal_log(enum mcp_internal_log_level level, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	g_log_fn(level, fmt, args);
	va_end(args);
}
