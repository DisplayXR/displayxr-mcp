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

#include <cjson/cJSON.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
	        "  --target auto      try the shell, then a unique PID session (default);\n"
	        "                     with no unique session, run as the workspace aggregator\n"
	        "                     (diagnostics exposed) so apps started later still join\n"
	        "  --target workspace aggregate the shell + every per-PID app session behind one\n"
	        "                     MCP connection with <app-id>__<tool> namespacing; membership\n"
	        "                     mirrors live (apps joining/leaving emit tools/list_changed)\n"
	        "  --target <role>    attach to a named MCP endpoint (e.g. 'shell')\n"
	        "                     resolves to /tmp/displayxr-mcp-<role>.sock or \\\\.\\pipe\\displayxr-mcp-<role>\n"
	        "  --target pid:N     attach to a specific in-process server (handle apps)\n"
	        "  --pid N | auto     back-compat form of --target pid:N / --target auto\n"
	        "  --expose-diagnostics  (workspace mode) also expose DIAGNOSTIC-group tools\n"
	        "  --list             print discovered sessions and exit\n"
	        "displayxr-service hosts no MCP endpoint: MCP servers run inside each OpenXR app\n"
	        "process (pid:N) and in the shell, so there is no 'service' target.\n",
	        argv0);
}

/*!
 * How long the bridge keeps waiting, after stdin EOF, for the replies to
 * requests it has already forwarded. Bounds the exit when a server never
 * answers; a server that closes the connection ends the wait at once.
 */
#define DRAIN_TIMEOUT_MS 30000

/*!
 * Which pump finished first. The bridge ends when EITHER side closes:
 * stdin EOF (the client went away) or the server connection closing
 * (the app exited / stopped its server). Waiting on stdin alone left
 * the adapter up, silently, with nothing behind it.
 *
 * On stdin EOF the bridge first drains: it waits until every request it
 * forwarded has had its reply written to stdout (`outstanding` == 0), so
 * a client that writes its requests and closes stdin straight away
 * (`echo ... | displayxr-mcp`) still gets every answer.
 */
struct pump_done
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool up_done;     //!< stdin -> conn pump returned.
	bool down_done;   //!< conn -> stdout pump returned.
	long outstanding; //!< Requests forwarded upstream whose reply is not yet on stdout.
};

/*!
 * Whether one message body owes, or is, a reply; feeds the drain on
 * stdin EOF (bodies still pass through unchanged).
 *
 * Client -> server: the server answers every JSON object that carries an
 * id (null included; a missing or bad method gets an "invalid request"
 * error) and nothing else: no reply to a notification, a batch, or a
 * body that does not parse (that error carries no id).
 *
 * Server -> client: a reply is an object with an id and no method (an
 * object with both is a server-initiated request).
 */
static bool
owes_reply(const char *body, size_t len)
{
	cJSON *root = cJSON_ParseWithLength(body, len);
	bool r = cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "id") != NULL;
	cJSON_Delete(root);
	return r;
}

static bool
is_reply(const char *body, size_t len)
{
	cJSON *root = cJSON_ParseWithLength(body, len);
	bool r = cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "id") != NULL &&
	         !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "method"));
	cJSON_Delete(root);
	return r;
}

static void
add_outstanding(struct pump_done *done, long delta)
{
	if (delta == 0) {
		return;
	}
	pthread_mutex_lock(&done->mutex);
	done->outstanding += delta;
	if (done->outstanding < 0) {
		done->outstanding = 0; // A reply we did not count.
	}
	pthread_cond_broadcast(&done->cond);
	pthread_mutex_unlock(&done->mutex);
}

// Thread args: pump stdin→conn or conn→stdout.
struct pump_args
{
	struct mcp_conn *conn;
	bool stdin_to_conn;
	struct dxr_stdio_framing_latch *latch;
	struct pump_done *done;
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
			long requests = owes_reply(body, len) ? 1 : 0;
			// Count before the write: the reply can land before it returns.
			add_outstanding(a->done, requests);
			bool ok = dxr_conn_write_frame(a->conn, body, len);
			free(body);
			if (!ok) {
				add_outstanding(a->done, -requests); // Never reached the server.
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
			long replies = is_reply(body, len) ? 1 : 0;
			size_t out_len = 0;
			char *out = dxr_stdio_encode(f, body, len, &out_len);
			free(body);
			bool ok = out != NULL && dxr_stdio_write_stdout(out, out_len);
			free(out);
			if (!ok) {
				break;
			}
			// Only once the reply is on stdout: the drain may exit right after.
			add_outstanding(a->done, -replies);
		}
	}
	pthread_mutex_lock(&a->done->mutex);
	if (a->stdin_to_conn) {
		a->done->up_done = true;
	} else {
		a->done->down_done = true;
	}
	pthread_cond_broadcast(&a->done->cond);
	pthread_mutex_unlock(&a->done->mutex);
	return NULL;
}

/*!
 * displayxr-service hosts no MCP endpoint: the servers run inside each
 * OpenXR app process (per-PID) and in the shell. `service` was a target
 * before the MCP server moved out of the service; it is never probed
 * (an endpoint under that name could only be a stale one from an old
 * install, which the workspace aggregator also refuses). An explicit
 * `--target service` gets this explanation instead of "cannot connect".
 */
static void
explain_no_service_endpoint(void)
{
	fprintf(stderr,
	        "displayxr-mcp: displayxr-service hosts no MCP endpoint, with or without an app open.\n"
	        "  MCP servers run inside each OpenXR app process and in the shell.\n"
	        "  Use --target auto (the shell, else the one running app, else the workspace\n"
	        "  aggregator), --target workspace (every app, joining as they start),\n"
	        "  --target pid:N (one app), or --target shell. --list shows what is running.\n");
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
		static const char *known_roles[] = {"shell"};
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
		// Try the shell first (the agent control surface for spatial
		// workspaces), then a unique PID session. Never `service`: see
		// explain_no_service_endpoint().
		static const char *roles[] = {"shell"};
		for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]) && conn == NULL; i++) {
			conn = mcp_conn_connect_named(roles[i]);
			if (conn != NULL) {
				connected_label = roles[i];
			}
		}
		if (conn == NULL) {
			long pids[64];
			size_t n = mcp_enumerate_sessions(pids, 64);
			if (n != 1) {
				// No unique session to bridge 1:1. Exiting here made MCP
				// clients that spawn the adapter once at startup (Claude
				// Code) show the server as failed whenever no app was
				// running yet, or when several were, until a manual
				// reconnect. Serve as the workspace aggregator instead: it
				// answers initialize itself, picks up apps as they start
				// and emits tools/list_changed. Tools are then named
				// <app-id>__<tool>. Diagnostics stay exposed, as they are
				// over a 1:1 pid bridge.
				fprintf(stderr,
				        "displayxr-mcp: %zu app sessions and no shell endpoint; "
				        "serving as the workspace aggregator (apps join as they start)\n",
				        n);
				return workspace_aggregator_run(true);
			}
			conn = mcp_conn_connect(pids[0]);
			static char buf[32];
			snprintf(buf, sizeof(buf), "pid %ld", pids[0]);
			connected_label = buf;
		}
	} else if (strcmp(target_arg, "service") == 0) {
		explain_no_service_endpoint();
		return 1;
	} else {
		// Treat any other token as a role name (e.g. "shell", or a
		// third-party workspace controller's role string).
		conn = mcp_conn_connect_named(target_arg);
		connected_label = target_arg;
	}

	if (conn == NULL) {
		fprintf(stderr, "displayxr-mcp: cannot connect to %s\n", connected_label ? connected_label : target_arg);
		return 1;
	}

	static struct dxr_stdio_framing_latch latch;
	dxr_stdio_framing_latch_init(&latch);
	static struct pump_done done;
	pthread_mutex_init(&done.mutex, NULL);
	pthread_cond_init(&done.cond, NULL);
	struct pump_args up = {.conn = conn, .stdin_to_conn = true, .latch = &latch, .done = &done};
	struct pump_args down = {.conn = conn, .stdin_to_conn = false, .latch = &latch, .done = &done};

	pthread_t t_up, t_down;
	pthread_create(&t_up, NULL, pump_thread, &up);
	pthread_create(&t_down, NULL, pump_thread, &down);

	pthread_mutex_lock(&done.mutex);
	while (!done.up_done && !done.down_done) {
		pthread_cond_wait(&done.cond, &done.mutex);
	}
	bool server_closed = done.down_done;
	pthread_mutex_unlock(&done.mutex);

	if (server_closed) {
		// The server side closed (or stdout failed). The stdin pump is
		// parked in a blocking read of stdin that cannot be woken
		// portably; returning from main ends the process, which is the
		// point: the client sees EOF on our stdout and can reconnect.
		fprintf(stderr, "displayxr-mcp: %s closed the connection; exiting\n", connected_label);
		fflush(stdout);
		return 0;
	}

	// The stdin side ended (client closed stdin). Answer what was already
	// asked first: wait until every forwarded request has its reply on
	// stdout, the server closes, or the drain timeout passes.
	pthread_join(t_up, NULL);
	struct timespec deadline;
	(void)timespec_get(&deadline, TIME_UTC); // Portable; see mcp_capture.c.
	deadline.tv_sec += DRAIN_TIMEOUT_MS / 1000;
	deadline.tv_nsec += (DRAIN_TIMEOUT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&done.mutex);
	while (done.outstanding > 0 && !done.down_done) {
		if (pthread_cond_timedwait(&done.cond, &done.mutex, &deadline) != 0) {
			fprintf(stderr, "displayxr-mcp: stdin closed; %ld repl%s still pending after %d ms, exiting\n",
			        done.outstanding, done.outstanding == 1 ? "y" : "ies", DRAIN_TIMEOUT_MS);
			break;
		}
	}
	pthread_mutex_unlock(&done.mutex);

	// Abort the connection so the conn-side thread unblocks its read.
	// Unblock a writer still waiting for the framing (client sent
	// nothing) too.
	dxr_stdio_framing_latch_set(&latch, DXR_STDIO_FRAMING_NDJSON);
	mcp_conn_abort(conn);
	pthread_join(t_down, NULL);
	mcp_conn_close(conn);
	return 0;
}
