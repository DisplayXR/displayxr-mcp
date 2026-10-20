// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stdio ↔ per-PID-socket/pipe bridge for the DisplayXR MCP server.
 *
 * Two blocking threads pump messages in opposite directions. The agent
 * side (stdio) speaks the MCP stdio transport — newline-delimited JSON —
 * or, auto-detected, the legacy Content-Length framing (see
 * stdio_framing.h); the server side (unix domain socket on POSIX, named
 * pipe on Windows) is always Content-Length-framed. Message bodies pass
 * through unparsed.
 */

#include "stdio_framing.h"
#include "workspace_aggregator.h"

#include "displayxr_mcp/mcp_transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static void
usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [--target <auto|workspace|<role>|pid:N>] [--expose-diagnostics] | --pid <N|auto> | --list\n"
	        "  --target auto      try known roles (shell, service), then unique PID session (default)\n"
	        "  --target workspace aggregate the shell + every per-PID app session behind one\n"
	        "                     MCP connection with <app-id>__<tool> namespacing; membership\n"
	        "                     mirrors live (apps joining/leaving emit tools/list_changed)\n"
	        "  --target <role>    attach to a named MCP endpoint (e.g. 'shell', 'service')\n"
	        "                     resolves to /tmp/displayxr-mcp-<role>.sock or \\\\.\\pipe\\displayxr-mcp-<role>\n"
	        "  --target pid:N     attach to a specific in-process server (handle apps)\n"
	        "  --pid N | auto     back-compat form of --target pid:N / --target auto\n"
	        "  --expose-diagnostics  (workspace mode) also expose DIAGNOSTIC-group tools\n"
	        "  --list             print discovered sessions and exit\n",
	        argv0);
}

// Thread args: pump stdin→conn or conn→stdout.
struct pump_args
{
	struct mcp_conn *conn;
	bool stdin_to_conn;
	struct dxr_stdio_framing_latch *latch;
};

static void *
pump_thread(void *arg)
{
	struct pump_args *a = arg;
	if (a->stdin_to_conn) {
		struct dxr_stdio_reader reader;
		dxr_stdio_reader_init(&reader, dxr_stdio_read_stdin, NULL);
		for (;;) {
			size_t len = 0;
			char *body = dxr_stdio_reader_next(&reader, &len);
			if (body == NULL) {
				break;
			}
			dxr_stdio_framing_latch_set(a->latch, reader.framing);
			bool ok = dxr_conn_write_frame(a->conn, body, len);
			free(body);
			if (!ok) {
				break;
			}
		}
		dxr_stdio_reader_fini(&reader);
	} else {
		for (;;) {
			size_t len = 0;
			char *body = dxr_conn_read_frame(a->conn, &len);
			if (body == NULL) {
				break;
			}
			// Reply in the framing the client used; blocks until the
			// client's first message has fixed it.
			enum dxr_stdio_framing f = dxr_stdio_framing_latch_wait(a->latch);
			size_t out_len = 0;
			char *out = dxr_stdio_encode(f, body, len, &out_len);
			free(body);
			bool ok = out != NULL && dxr_stdio_write_stdout(out, out_len);
			free(out);
			if (!ok) {
				break;
			}
		}
	}
	return NULL;
}

int
main(int argc, char **argv)
{
#ifdef _WIN32
	// Disable CRLF translation: the legacy framing counts bytes, and
	// ndjson lines must end in a bare \n.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	const char *target_arg = NULL; // "auto", "workspace", "service", or "pid:N"
	bool list_mode = false;
	bool expose_diagnostics = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
			target_arg = argv[++i];
		} else if (strcmp(argv[i], "--expose-diagnostics") == 0) {
			expose_diagnostics = true;
		} else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
			// Back-compat: --pid N → --target pid:N ; --pid auto → --target auto.
			const char *v = argv[++i];
			static char buf[64];
			if (strcmp(v, "auto") == 0) {
				target_arg = "auto";
			} else {
				snprintf(buf, sizeof(buf), "pid:%s", v);
				target_arg = buf;
			}
		} else if (strcmp(argv[i], "--list") == 0) {
			list_mode = true;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (list_mode) {
		long pids[64];
		size_t n = mcp_enumerate_sessions(pids, 64);
		// Probe known role endpoints — a trial connect is the cheapest
		// cross-platform existence check. Add new roles as the ecosystem
		// adds them; unknown roles can be enumerated by external tools
		// inspecting /tmp/displayxr-mcp-*.sock directly.
		static const char *known_roles[] = {"shell", "service"};
		for (size_t i = 0; i < sizeof(known_roles) / sizeof(known_roles[0]); i++) {
			struct mcp_conn *probe = mcp_conn_connect_named(known_roles[i]);
			if (probe != NULL) {
				printf("%s\n", known_roles[i]);
				mcp_conn_close(probe);
			}
		}
		for (size_t i = 0; i < n; i++) {
			printf("%ld\n", (long)pids[i]);
		}
		return 0;
	}

	if (target_arg == NULL) {
		target_arg = "auto";
	}

	if (strcmp(target_arg, "workspace") == 0) {
		// MCP-terminating aggregator mode — never reaches the 1:1
		// byte-shuttle below.
		return workspace_aggregator_run(expose_diagnostics);
	}

	struct mcp_conn *conn = NULL;
	const char *connected_label = NULL;

	if (strncmp(target_arg, "pid:", 4) == 0) {
		long pid = (long)strtol(target_arg + 4, NULL, 10);
		if (pid <= 0) {
			usage(argv[0]);
			return 2;
		}
		conn = mcp_conn_connect(pid);
		static char buf[32];
		snprintf(buf, sizeof(buf), "pid %ld", pid);
		connected_label = buf;
	} else if (strcmp(target_arg, "auto") == 0) {
		// Try known roles in priority order: shell first (the agent
		// control surface for spatial workspaces), then service (legacy /
		// non-shell deployments), then a unique PID session.
		static const char *roles[] = {"shell", "service"};
		for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]) && conn == NULL; i++) {
			conn = mcp_conn_connect_named(roles[i]);
			if (conn != NULL) {
				connected_label = roles[i];
			}
		}
		if (conn == NULL) {
			long pids[64];
			size_t n = mcp_enumerate_sessions(pids, 64);
			if (n == 0) {
				fprintf(stderr, "displayxr-mcp: no running MCP sessions found\n");
				return 1;
			}
			if (n > 1) {
				fprintf(stderr, "displayxr-mcp: %zu sessions found, pass --target pid:N explicitly\n", n);
				return 1;
			}
			conn = mcp_conn_connect(pids[0]);
			static char buf[32];
			snprintf(buf, sizeof(buf), "pid %ld", pids[0]);
			connected_label = buf;
		}
	} else {
		// Treat any other token as a role name (e.g. "shell", "service",
		// or a third-party workspace controller's role string).
		conn = mcp_conn_connect_named(target_arg);
		connected_label = target_arg;
	}

	if (conn == NULL) {
		fprintf(stderr, "displayxr-mcp: cannot connect to %s\n", connected_label ? connected_label : target_arg);
		return 1;
	}

	static struct dxr_stdio_framing_latch latch;
	dxr_stdio_framing_latch_init(&latch);
	struct pump_args up = {.conn = conn, .stdin_to_conn = true, .latch = &latch};
	struct pump_args down = {.conn = conn, .stdin_to_conn = false, .latch = &latch};

	pthread_t t_up, t_down;
	pthread_create(&t_up, NULL, pump_thread, &up);
	pthread_create(&t_down, NULL, pump_thread, &down);

	// When the stdin-side thread exits (client closed stdin), abort the
	// connection so the conn-side thread unblocks its read. Unblock a
	// writer still waiting for the framing (client sent nothing) too.
	pthread_join(t_up, NULL);
	dxr_stdio_framing_latch_set(&latch, DXR_STDIO_FRAMING_NDJSON);
	mcp_conn_abort(conn);
	pthread_join(t_down, NULL);
	mcp_conn_close(conn);
	return 0;
}
