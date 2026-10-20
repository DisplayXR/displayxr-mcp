// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*
 * Standalone smoke test for the displayxr_mcp framework.
 *
 * Spins up an MCP server on /tmp/displayxr-mcp-smoke.sock with a single
 * mock tool (mock_list_windows) that returns fixed window data, then
 * forks the displayxr-mcp adapter and exercises:
 *   - initialize
 *   - tools/list
 *   - tools/call mock_list_windows
 *   - tools/call mock_fail (a tool error: result.isError, message kept)
 *
 * Runs twice: once speaking the MCP stdio transport (newline-delimited
 * JSON — what Claude Code and every spec-conformant client send), once
 * speaking the legacy Content-Length framing; the adapter auto-detects
 * and must reply in the framing it was spoken to.
 *
 * Verifies the JSON-RPC envelope, tool dispatch, and stdio↔socket
 * pump. This is the same wire path Phase B (in shell-pvt) and Phase A
 * (in the runtime) use — only the tool implementations differ.
 *
 * Build: enabled by -DDISPLAYXR_MCP_BUILD_TESTS=ON.
 * Run:   ctest -V (or ./build/tests/displayxr_mcp_smoke)
 */

#include <displayxr_mcp/mcp_server.h>

#include <cjson/cJSON.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static cJSON *
mock_list_windows(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *arr = cJSON_CreateArray();
	cJSON *w = cJSON_CreateObject();
	cJSON_AddNumberToObject(w, "id", 42);
	cJSON_AddStringToObject(w, "name", "mock_app");
	cJSON_AddBoolToObject(w, "focused", true);
	cJSON_AddItemToArray(arr, w);
	return arr;
}

static const struct mcp_tool MOCK_TOOL = {
    .name = "mock_list_windows",
    .description = "Smoke-test stand-in. Returns one fake window.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_list_windows,
};

// Fails the way an app tool does (XR_DXR_mcp_tools, a throwing handler):
// a result object flagged isError, carrying the message.
static cJSON *
mock_fail(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "isError");
	cJSON_AddStringToObject(o, "error", "unknown behavior 'nope'");
	return o;
}

static const struct mcp_tool MOCK_FAIL_TOOL = {
    .name = "mock_fail",
    .description = "Smoke-test stand-in. Always fails as a tool error.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_fail,
};

// result.isError of a tools/call response: 1 = true, 0 = absent/false, -1 = bad body.
static int
result_is_error(const char *body)
{
	cJSON *root = cJSON_Parse(body);
	if (root == NULL) {
		return -1;
	}
	const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
	int r = result == NULL ? -1 : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")) ? 1 : 0;
	cJSON_Delete(root);
	return r;
}

static int
expect_substring(const char *haystack, const char *needle, const char *label)
{
	if (strstr(haystack, needle) == NULL) {
		fprintf(stderr, "FAIL: %s — expected to find '%s' in:\n%s\n", label, needle, haystack);
		return 1;
	}
	printf("  ok: %s\n", label);
	return 0;
}

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

// Read one Content-Length-framed body. Caller must free *out.
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

// ndjson: one message per line. Writes the body in two halves with a
// pause between them so the adapter sees a message split across reads.
static int
send_line_split(FILE *f, const char *body)
{
	size_t len = strlen(body);
	size_t half = len / 2;
	if (fwrite(body, 1, half, f) != half) {
		return -1;
	}
	fflush(f);
	usleep(50 * 1000);
	if (fwrite(body + half, 1, len - half, f) != len - half || fputc('\n', f) == EOF) {
		return -1;
	}
	fflush(f);
	return 0;
}

// Read one ndjson line (without the terminator). Caller must free *out.
static int
recv_line(FILE *f, char **out)
{
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		return -1;
	}
	int c;
	while ((c = fgetc(f)) != EOF && c != '\n') {
		if (n + 1 == cap) {
			char *nb = realloc(buf, cap *= 2);
			if (nb == NULL) {
				free(buf);
				return -1;
			}
			buf = nb;
		}
		buf[n++] = (char)c;
	}
	if (c == EOF || n == 0) {
		free(buf);
		return -1;
	}
	buf[n] = '\0';
	*out = buf;
	return 0;
}

static pid_t
spawn_adapter(const char *adapter, FILE **to, FILE **from)
{
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0 || pipe(from_child) != 0) {
		perror("pipe");
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);
		close(to_child[1]);
		close(from_child[0]);
		execl(adapter, adapter, "--target", "smoke", (char *)NULL);
		perror("exec");
		_exit(127);
	}
	close(to_child[0]);
	close(from_child[1]);
	*to = fdopen(to_child[1], "w");
	*from = fdopen(from_child[0], "r");
	return pid;
}

// "你好，世界" as UTF-8.
#define ZH "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c"

static int
run_ndjson(const char *adapter)
{
	printf("--- ndjson (MCP stdio transport) ---\n");
	FILE *out = NULL, *in = NULL;
	pid_t pid = spawn_adapter(adapter, &out, &in);
	if (pid <= 0) {
		return 1;
	}
	int failures = 0;
	char *line = NULL;

	send_line_split(out, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
	                     "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
	                     "\"clientInfo\":{\"name\":\"smoke\",\"version\":\"0\"}}}");
	if (recv_line(in, &line) != 0) {
		fprintf(stderr, "FAIL: ndjson: no initialize response line\n");
		failures++;
	} else {
		failures += expect_substring(line, "\"protocolVersion\"", "ndjson initialize: protocolVersion");
		if (strncmp(line, "{", 1) != 0) {
			fprintf(stderr, "FAIL: ndjson: reply is not a bare JSON line: %s\n", line);
			failures++;
		} else {
			printf("  ok: ndjson initialize: reply is a bare JSON line (no Content-Length)\n");
		}
		free(line);
	}
	// Notification: no reply expected.
	send_line_split(out, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

	send_line_split(out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
	if (recv_line(in, &line) != 0) {
		fprintf(stderr, "FAIL: ndjson: no tools/list response line\n");
		failures++;
	} else {
		failures += expect_substring(line, "\"id\":2", "ndjson tools/list: id 2");
		failures += expect_substring(line, "\"mock_list_windows\"", "ndjson tools/list: mock tool present");
		free(line);
	}

	send_line_split(out, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
	                     "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"" ZH "\"}}}");
	if (recv_line(in, &line) != 0) {
		fprintf(stderr, "FAIL: ndjson: no tools/call response line\n");
		failures++;
	} else {
		failures += expect_substring(line, "\"id\":3", "ndjson tools/call: id 3");
		failures += expect_substring(line, ZH, "ndjson tools/call echo: UTF-8 (Chinese) string round-trips");
		free(line);
	}

	fclose(out);
	fclose(in);
	int status = 0;
	waitpid(pid, &status, 0);
	return failures;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <path-to-displayxr-mcp-adapter>\n", argv[0]);
		return 2;
	}
	const char *adapter = argv[1];
	alarm(60); // Hard kill on hang (e.g. an adapter that never answers ndjson).
	signal(SIGPIPE, SIG_IGN);

	// Start the server.
	setenv("DISPLAYXR_MCP", "1", 1);
	mcp_server_register_tool(&MOCK_TOOL);
	mcp_server_register_tool(&MOCK_FAIL_TOOL);
	mcp_server_maybe_start_named("smoke");

	// Give the server thread a beat to bind.
	usleep(100 * 1000);

	// Fork the adapter as a subprocess: parent writes JSON-RPC to its
	// stdin, reads from its stdout. Two pipes wired to popen-style.
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0 || pipe(from_child) != 0) {
		perror("pipe");
		return 1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);
		close(to_child[1]);
		close(from_child[0]);
		execl(adapter, adapter, "--target", "smoke", (char *)NULL);
		perror("exec");
		_exit(127);
	}
	close(to_child[0]);
	close(from_child[1]);
	FILE *out = fdopen(to_child[1], "w");
	FILE *in = fdopen(from_child[0], "r");

	int failures = 0;
	printf("--- Content-Length (legacy framing) ---\n");

	// 1. initialize
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
	char *body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no initialize response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"protocolVersion\"", "initialize: protocolVersion");
		failures += expect_substring(body, "\"displayxr-mcp\"", "initialize: server name");
		free(body);
	}

	// 2. tools/list
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
	body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no tools/list response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"mock_list_windows\"", "tools/list: mock tool present");
		failures += expect_substring(body, "\"echo\"", "tools/list: built-in echo present");
		free(body);
	}

	// 3. tools/call mock_list_windows
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
	                "\"params\":{\"name\":\"mock_list_windows\",\"arguments\":{}}}");
	body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no tools/call response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"mock_app\"", "tools/call: returned mock window name");
		failures += expect_substring(body, "\"id\":42", "tools/call: returned mock window id");
		if (result_is_error(body) != 0) {
			fprintf(stderr, "FAIL: tools/call: a successful result must not carry isError:\n%s\n", body);
			failures++;
		} else {
			printf("  ok: tools/call: success carries no isError\n");
		}
		free(body);
	}

	// 4. tools/call mock_fail — a tool error is a RESULT with isError:true and
	// the message, never a JSON-RPC error.
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
	                "\"params\":{\"name\":\"mock_fail\",\"arguments\":{}}}");
	body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no tools/call (mock_fail) response\n");
		failures++;
	} else {
		failures += expect_substring(body, "unknown behavior 'nope'", "tools/call error: message kept");
		if (strstr(body, "\"error\":{") != NULL || result_is_error(body) != 1) {
			fprintf(stderr, "FAIL: tools/call error: expected result.isError=true, got:\n%s\n", body);
			failures++;
		} else {
			printf("  ok: tools/call error: result.isError=true\n");
		}
		free(body);
	}

	// Tear down: close adapter stdin so it exits cleanly.
	fclose(out);
	fclose(in);
	int status = 0;
	waitpid(pid, &status, 0);

	failures += run_ndjson(adapter);

	mcp_server_stop();

	if (failures == 0) {
		printf("=== smoke test PASSED ===\n");
		return 0;
	}
	printf("=== smoke test FAILED (%d) ===\n", failures);
	return 1;
}
