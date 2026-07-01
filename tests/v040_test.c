// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*
 * v0.4.0 feature test: dynamic tool registry, tool groups, app id,
 * multi-client transport, and the workspace aggregator.
 *
 * The test process hosts a per-PID MCP server (the same shape a handle
 * app has) with mock app tools, then exercises:
 *
 *   A. Two concurrent direct clients (multi-client transport);
 *      initialize advertises listChanged + appId; tools/list carries
 *      _meta displayxr/group + displayxr/appId; late register and
 *      unregister broadcast notifications/tools/list_changed to both.
 *   B. The forked `displayxr-mcp --target workspace` aggregator:
 *      discovers the pid session, prefixes tools with the app id
 *      (mockapp__*), hides DIAGNOSTIC tools by default, exposes
 *      CAPTURE, routes tools/call with id rewriting, serves
 *      workspace__list_apps, and relays list_changed on late
 *      registration.
 *
 * Build: -DDISPLAYXR_MCP_BUILD_TESTS=ON. Run: ctest -V.
 */

#include <displayxr_mcp/mcp_server.h>
#include <displayxr_mcp/mcp_transport.h>

#include <cjson/cJSON.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;

static void
check(bool ok, const char *label)
{
	if (ok) {
		printf("  ok: %s\n", label);
	} else {
		fprintf(stderr, "FAIL: %s\n", label);
		g_failures++;
	}
}

static void
check_substr(const char *haystack, const char *needle, const char *label)
{
	if (haystack != NULL && strstr(haystack, needle) != NULL) {
		printf("  ok: %s\n", label);
	} else {
		fprintf(stderr, "FAIL: %s — expected '%s' in:\n%s\n", label, needle,
		        haystack != NULL ? haystack : "(null)");
		g_failures++;
	}
}

// ---------------------------------------------------------------- mock tools

static cJSON *
mock_play_pause(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "played", true);
	return r;
}

static const struct mcp_tool PLAY_PAUSE_TOOL = {
    .name = "play_pause",
    .description = "Mock app tool.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_play_pause,
    .group = MCP_TOOL_GROUP_APP,
};

static cJSON *
mock_capture(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "png", "deadbeef");
	return r;
}

static const struct mcp_tool CAPTURE_TOOL = {
    .name = "capture_frame",
    .description = "Mock capture tool.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_capture,
    .group = MCP_TOOL_GROUP_CAPTURE,
};

static cJSON *
mock_late(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	return cJSON_CreateObject();
}

static const struct mcp_tool LATE_TOOL = {
    .name = "late_tool",
    .description = "Registered after clients connected.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_late,
    .group = MCP_TOOL_GROUP_APP,
};

// ---------------------------------------------------------------- conn framing

static bool
conn_send(struct mcp_conn *c, const char *body)
{
	char header[64];
	int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", strlen(body));
	return hlen > 0 && mcp_conn_write(c, header, (size_t)hlen) &&
	       mcp_conn_write(c, body, strlen(body));
}

// Read one frame body from a conn. Caller frees. NULL on EOF.
static char *
conn_recv(struct mcp_conn *c)
{
	char line[256];
	size_t n = 0, content_length = 0;
	bool have_length = false, in_line = true;
	while (in_line) {
		n = 0;
		for (;;) {
			char ch;
			if (!mcp_conn_read(c, &ch, 1)) {
				return NULL;
			}
			if (n + 1 < sizeof(line)) {
				line[n++] = ch;
			}
			if (ch == '\n') {
				break;
			}
		}
		line[n] = '\0';
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_length = (size_t)strtoul(line + 15, NULL, 10);
			have_length = true;
		}
	}
	if (!have_length || content_length == 0) {
		return NULL;
	}
	char *body = malloc(content_length + 1);
	if (body == NULL || !mcp_conn_read(c, body, content_length)) {
		free(body);
		return NULL;
	}
	body[content_length] = '\0';
	return body;
}

// ---------------------------------------------------------------- stdio framing (adapter child)

static int
send_frame(FILE *f, const char *body)
{
	size_t len = strlen(body);
	if (fprintf(f, "Content-Length: %zu\r\n\r\n", len) < 0) {
		return -1;
	}
	if (fwrite(body, 1, len, f) != len) {
		return -1;
	}
	fflush(f);
	return 0;
}

static int
recv_frame(FILE *f, char **out)
{
	char line[256];
	size_t content_length = 0;
	bool have_length = false;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_length = (size_t)strtoul(line + 15, NULL, 10);
			have_length = true;
		}
	}
	if (!have_length || content_length == 0) {
		return -1;
	}
	*out = malloc(content_length + 1);
	if (*out == NULL) {
		return -1;
	}
	if (fread(*out, 1, content_length, f) != content_length) {
		free(*out);
		return -1;
	}
	(*out)[content_length] = '\0';
	return 0;
}

// ---------------------------------------------------------------- section A

static void
section_a_direct_multiclient(void)
{
	printf("--- A: direct multi-client + dynamic registry ---\n");
	long pid = mcp_self_pid();

	struct mcp_conn *c1 = mcp_conn_connect(pid);
	struct mcp_conn *c2 = mcp_conn_connect(pid);
	check(c1 != NULL, "A: first client connected");
	check(c2 != NULL, "A: second concurrent client connected");
	if (c1 == NULL || c2 == NULL) {
		return;
	}

	// Both clients handshake.
	conn_send(c1, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
	char *r = conn_recv(c1);
	check_substr(r, "\"listChanged\":true", "A: initialize advertises listChanged");
	check_substr(r, "\"appId\":\"mockapp\"", "A: initialize carries appId");
	free(r);
	conn_send(c2, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
	r = conn_recv(c2);
	check(r != NULL, "A: second client initialize answered (multi-client serve)");
	free(r);

	// Groups + appId in tools/list.
	conn_send(c2, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
	r = conn_recv(c2);
	check_substr(r, "\"play_pause\"", "A: tools/list has mock app tool");
	check_substr(r, "\"displayxr/group\":\"app\"", "A: _meta group=app surfaced");
	check_substr(r, "\"displayxr/group\":\"capture\"", "A: _meta group=capture surfaced");
	check_substr(r, "\"displayxr/appId\":\"mockapp\"", "A: result _meta appId surfaced");
	free(r);

	// Late register → both clients get list_changed.
	mcp_server_register_tool(&LATE_TOOL);
	r = conn_recv(c1);
	check_substr(r, "notifications/tools/list_changed", "A: c1 got list_changed on register");
	free(r);
	r = conn_recv(c2);
	check_substr(r, "notifications/tools/list_changed", "A: c2 got list_changed on register");
	free(r);

	// Unregister → another broadcast.
	mcp_server_unregister_tool("late_tool");
	r = conn_recv(c1);
	check_substr(r, "notifications/tools/list_changed", "A: c1 got list_changed on unregister");
	free(r);

	mcp_conn_close(c1);
	mcp_conn_close(c2);
}

// ---------------------------------------------------------------- section B

static pid_t
fork_adapter(const char *adapter, FILE **to, FILE **from)
{
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0 || pipe(from_child) != 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);
		close(to_child[1]);
		close(from_child[0]);
		execl(adapter, adapter, "--target", "workspace", (char *)NULL);
		perror("exec");
		_exit(127);
	}
	close(to_child[0]);
	close(from_child[1]);
	*to = fdopen(to_child[1], "w");
	*from = fdopen(from_child[0], "r");
	return pid;
}

/*!
 * Poll tools/list (skipping interleaved notifications) until @p needle
 * appears or ~@p tries half-seconds elapse. Returns the matching body
 * (caller frees) or NULL.
 */
static char *
await_tool(FILE *to, FILE *from, int *next_id, const char *needle, int tries)
{
	for (int i = 0; i < tries; i++) {
		char req[128];
		int id = (*next_id)++;
		snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/list\"}", id);
		send_frame(to, req);
		// Consume frames until this request's response.
		for (;;) {
			char *body = NULL;
			if (recv_frame(from, &body) != 0) {
				return NULL;
			}
			char idtok[32];
			snprintf(idtok, sizeof(idtok), "\"id\":%d", id);
			bool is_resp = strstr(body, idtok) != NULL;
			if (is_resp) {
				if (strstr(body, needle) != NULL) {
					return body;
				}
				free(body);
				break; // Right response, tool not there yet.
			}
			free(body); // Notification or stale; keep reading.
		}
		usleep(500 * 1000);
	}
	return NULL;
}

static void
section_b_aggregator(const char *adapter)
{
	printf("--- B: workspace aggregator ---\n");
	FILE *to = NULL, *from = NULL;
	pid_t child = fork_adapter(adapter, &to, &from);
	check(child > 0, "B: aggregator forked");
	if (child <= 0) {
		return;
	}
	int next_id = 1;

	send_frame(to, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{}}");
	char *r = NULL;
	if (recv_frame(from, &r) == 0) {
		check_substr(r, "displayxr-mcp-workspace", "B: aggregator serverInfo");
		check_substr(r, "\"listChanged\":true", "B: aggregator advertises listChanged");
		free(r);
	} else {
		check(false, "B: no initialize response");
	}
	send_frame(to, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

	// Membership poll runs ~1 Hz; wait for our pid session to surface.
	r = await_tool(to, from, &next_id, "\"mockapp__play_pause\"", 16);
	check(r != NULL, "B: app tool surfaced with app-id prefix (mockapp__play_pause)");
	if (r != NULL) {
		check_substr(r, "\"workspace__list_apps\"", "B: workspace meta-tool present");
		check_substr(r, "\"mockapp__capture_frame\"", "B: capture group exposed");
		check(strstr(r, "\"mockapp__echo\"") == NULL,
		      "B: diagnostic tools hidden by default (no mockapp__echo)");
		check_substr(r, "\"displayxr/source\":\"mockapp\"", "B: _meta source tag added");
		free(r);
	}

	// Routed call with id rewriting.
	{
		char req[256];
		int id = next_id++;
		snprintf(req, sizeof(req),
		         "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
		         "\"params\":{\"name\":\"mockapp__play_pause\",\"arguments\":{}}}",
		         id);
		send_frame(to, req);
		char idtok[32];
		snprintf(idtok, sizeof(idtok), "\"id\":%d", id);
		for (;;) {
			if (recv_frame(from, &r) != 0) {
				check(false, "B: no tools/call response");
				break;
			}
			if (strstr(r, idtok) != NULL) {
				check_substr(r, "\"played\":true", "B: routed call returned app result");
				check_substr(r, idtok, "B: agent id restored on response");
				free(r);
				break;
			}
			free(r);
		}
	}

	// workspace__list_apps.
	{
		char req[256];
		int id = next_id++;
		snprintf(req, sizeof(req),
		         "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
		         "\"params\":{\"name\":\"workspace__list_apps\",\"arguments\":{}}}",
		         id);
		send_frame(to, req);
		char idtok[32];
		snprintf(idtok, sizeof(idtok), "\"id\":%d", id);
		for (;;) {
			if (recv_frame(from, &r) != 0) {
				check(false, "B: no list_apps response");
				break;
			}
			if (strstr(r, idtok) != NULL) {
				check_substr(r, "\"mockapp\"", "B: list_apps shows the app prefix");
				free(r);
				break;
			}
			free(r);
		}
	}

	// Late registration relays list_changed upstream and the tool
	// becomes callable under the prefix.
	mcp_server_register_tool(&LATE_TOOL);
	r = await_tool(to, from, &next_id, "\"mockapp__late_tool\"", 10);
	check(r != NULL, "B: late-registered tool surfaced through aggregator");
	free(r);
	mcp_server_unregister_tool("late_tool");

	// Unknown prefix errors cleanly.
	{
		char req[256];
		int id = next_id++;
		snprintf(req, sizeof(req),
		         "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
		         "\"params\":{\"name\":\"nosuchapp__nope\",\"arguments\":{}}}",
		         id);
		send_frame(to, req);
		char idtok[32];
		snprintf(idtok, sizeof(idtok), "\"id\":%d", id);
		for (;;) {
			if (recv_frame(from, &r) != 0) {
				check(false, "B: no error response for unknown prefix");
				break;
			}
			if (strstr(r, idtok) != NULL) {
				check_substr(r, "\"error\"", "B: unknown prefix yields JSON-RPC error");
				free(r);
				break;
			}
			free(r);
		}
	}

	fclose(to);
	fclose(from);
	int status = 0;
	waitpid(child, &status, 0);
	check(WIFEXITED(status), "B: aggregator exited cleanly on stdin EOF");
}

// ---------------------------------------------------------------- main

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <path-to-displayxr-mcp-adapter>\n", argv[0]);
		return 2;
	}
	alarm(120); // Hard kill on hang — CI must never wedge.
	// Writes to a died adapter child must fail, not kill the test.
	signal(SIGPIPE, SIG_IGN);

	// Host a per-PID server, exactly the shape a handle app has.
	setenv("DISPLAYXR_MCP", "1", 1);
	mcp_server_register_tool(&PLAY_PAUSE_TOOL);
	mcp_server_register_tool(&CAPTURE_TOOL);
	mcp_server_set_app_id("mockapp");
	mcp_server_maybe_start();
	usleep(100 * 1000);

	section_a_direct_multiclient();
	section_b_aggregator(argv[1]);

	mcp_server_stop();

	if (g_failures == 0) {
		printf("=== v0.4.0 test PASSED ===\n");
		return 0;
	}
	printf("=== v0.4.0 test FAILED (%d) ===\n", g_failures);
	return 1;
}
